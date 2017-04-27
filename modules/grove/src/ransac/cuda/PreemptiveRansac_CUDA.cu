/**
 * grove: PreemptiveRansac_CUDA.cu
 * Copyright (c) Torr Vision Group, University of Oxford, 2017. All rights reserved.
 */

#include "ransac/cuda/PreemptiveRansac_CUDA.h"

#include <thrust/device_ptr.h>
#include <thrust/sort.h>

#include <itmx/MemoryBlockFactory.h>

#include "ransac/shared/PreemptiveRansac_Shared.h"

using namespace itmx;
using namespace tvgutil;

namespace grove {

//#################### CUDA KERNELS ####################

__global__ void ck_preemptive_ransac_compute_energies(const Keypoint3DColour *keypoints,
                                                      const ScorePrediction *predictions,
                                                      const int *inlierRasterIndices,
                                                      uint32_t nbInliers,
                                                      PoseCandidate *poseCandidates,
                                                      int nbCandidates)
{
  const int tId = threadIdx.x;
  const int threadsPerBlock = blockDim.x;
  const int candidateIdx = blockIdx.x;

  if (candidateIdx >= nbCandidates)
  {
    // Candidate has been culled.
    // Since the entire block returns, this does not cause troubles with the following __syncthreads()
    return;
  }

  PoseCandidate &currentCandidate = poseCandidates[candidateIdx];

  // Compute the energy for a strided subset of inliers.
  float localEnergy = preemptive_ransac_compute_candidate_energy(
      currentCandidate.cameraPose, keypoints, predictions, inlierRasterIndices, nbInliers, tId, threadsPerBlock);

  // The reduction is performed as in the following blog post:
  // https://devblogs.nvidia.com/parallelforall/faster-parallel-reductions-kepler

  // Reduce by shuffling down the local energies (localEnergy for thread 0 in the warp contains the sum for the warp).
  for (int offset = warpSize / 2; offset > 0; offset /= 2) localEnergy += __shfl_down(localEnergy, offset);

  // Thread 0 of each warp atomically updates the final energy.
  if ((threadIdx.x & (warpSize - 1)) == 0) atomicAdd(&currentCandidate.energy, localEnergy);

  __syncthreads(); // Wait for all threads in the block

  // tId 0 computes the final energy
  if (tId == 0) currentCandidate.energy = currentCandidate.energy / static_cast<float>(nbInliers);
}

template <typename RNG>
__global__ void ck_preemptive_ransac_generate_pose_candidates(const Keypoint3DColour *keypoints,
                                                              const ScorePrediction *predictions,
                                                              const Vector2i imgSize,
                                                              RNG *randomGenerators,
                                                              PoseCandidate *poseCandidates,
                                                              int *nbPoseCandidates,
                                                              int maxNbPoseCandidates,
                                                              bool useAllModesPerLeafInPoseHypothesisGeneration,
                                                              bool checkMinDistanceBetweenSampledModes,
                                                              float minDistanceBetweenSampledModes,
                                                              bool checkRigidTransformationConstraint,
                                                              float translationErrorMaxForCorrectPose)
{
  const int candidateIdx = blockIdx.x * blockDim.x + threadIdx.x;

  if (candidateIdx >= maxNbPoseCandidates) return;

  // Try to generate a candidate in a local variable.
  PoseCandidate candidate;

  bool valid = preemptive_ransac_generate_candidate(keypoints,
                                                    predictions,
                                                    imgSize,
                                                    randomGenerators[candidateIdx],
                                                    candidate,
                                                    useAllModesPerLeafInPoseHypothesisGeneration,
                                                    checkMinDistanceBetweenSampledModes,
                                                    minDistanceBetweenSampledModes,
                                                    checkRigidTransformationConstraint,
                                                    translationErrorMaxForCorrectPose);

  // If we succeeded, grab an unique index and store the candidate in the array.
  if (valid)
  {
    const int finalCandidateIdx = atomicAdd(nbPoseCandidates, 1);
    poseCandidates[finalCandidateIdx] = candidate;
  }
}

__global__ void ck_preemptive_ransac_init_random_generators(CUDARNG *randomGenerators, uint32_t nbStates, uint32_t seed)
{
  int idx = threadIdx.x + blockIdx.x * blockDim.x;

  if (idx >= nbStates) return;

  randomGenerators[idx].reset(seed, idx);
}

__global__ void ck_preemptive_ransac_reset_candidate_energies(PoseCandidate *poseCandidates, int nbPoseCandidates)
{
  const int candidateIdx = threadIdx.x;

  if (candidateIdx >= nbPoseCandidates)
  {
    return;
  }

  poseCandidates[candidateIdx].energy = 0.f;
}

template <bool useMask, typename RNG>
__global__ void ck_preemptive_ransac_sample_inliers(const Keypoint3DColour *keypointsData,
                                                    const ScorePrediction *predictionsData,
                                                    const Vector2i imgSize,
                                                    RNG *randomGenerators,
                                                    int *inlierIndices,
                                                    int *inlierCount,
                                                    int nbMaxSamples,
                                                    int *inlierMaskData = NULL)
{
  const int sampleIdx = blockIdx.x * blockDim.x + threadIdx.x;

  if (sampleIdx >= nbMaxSamples) return;

  // Try to sample the raster index of a valid keypoint which prediction has at least one modal cluster, using the mask
  // if necessary.
  const int sampledLinearIdx = preemptive_ransac_sample_inlier<useMask>(
      keypointsData, predictionsData, imgSize, randomGenerators[sampleIdx], inlierMaskData);

  // If the sampling succeeded grab a global index and store the keypoint index.
  if (sampledLinearIdx >= 0)
  {
    const int outIdx = atomicAdd(inlierCount, 1);
    inlierIndices[outIdx] = sampledLinearIdx;
  }
}

//#################### CONSTRUCTORS ####################

PreemptiveRansac_CUDA::PreemptiveRansac_CUDA() : PreemptiveRansac()
{
  MemoryBlockFactory &mbf = MemoryBlockFactory::instance();

  // Allocate memory blocks.
  m_nbPoseCandidates_device =
      mbf.make_block<int>(1); // Size 1, just to store a value that can be accessed from the GPU.
  m_nbSampledInliers_device = mbf.make_block<int>(1); // As above.
  m_randomGenerators = mbf.make_block<CUDARNG>(m_nbMaxPoseCandidates);

  // Default random seed.
  m_rngSeed = 42;

  // Reset RNGs.
  init_random();
}

//#################### PROTECTED VIRTUAL MEMBER FUNCTIONS ####################

void PreemptiveRansac_CUDA::compute_and_sort_energies()
{
  // Number of currently sampled inlier points, used to compute the energy.
  const size_t nbInliers = m_inliersIndicesBlock->dataSize;
  // Number of currently "valid" pose candidates.
  const size_t nbPoseCandidates = m_poseCandidates->dataSize;

  const Keypoint3DColour *keypoints = m_keypointsImage->GetData(MEMORYDEVICE_CUDA);
  const ScorePrediction *predictions = m_predictionsImage->GetData(MEMORYDEVICE_CUDA);
  // Indices of the sampled inliers.
  const int *inlierRasterIndices = m_inliersIndicesBlock->GetData(MEMORYDEVICE_CUDA);

  PoseCandidate *poseCandidates = m_poseCandidates->GetData(MEMORYDEVICE_CUDA);

  // First, reset the already computed energies. 1 block, a thread per candidate. Works up to 1024 candidates with
  // current GPUs.
  ck_preemptive_ransac_reset_candidate_energies<<<1, nbPoseCandidates>>>(poseCandidates, nbPoseCandidates);
  ORcudaKernelCheck;

  dim3 blockSize(128); // Threads to compute the energy for each candidate.
  dim3 gridSize(
      nbPoseCandidates); // Launch one block per candidate (in this way many blocks will exit immediately in later
                         // stages of P-RANSAC).
  ck_preemptive_ransac_compute_energies<<<gridSize, blockSize>>>(
      keypoints, predictions, inlierRasterIndices, nbInliers, poseCandidates, nbPoseCandidates);
  ORcudaKernelCheck;

  // Sort candidates by ascending energy using the operator <
  thrust::device_ptr<PoseCandidate> candidatesStart(poseCandidates);
  thrust::device_ptr<PoseCandidate> candidatesEnd(poseCandidates + nbPoseCandidates);
  thrust::sort(candidatesStart, candidatesEnd);
}

void PreemptiveRansac_CUDA::generate_pose_candidates()
{
  // The pose update phase needs keypoint and predictions on the host,
  // Performing the update call here since this function is called once per frame, while the update function is called
  // many times per
  // frame and keypoint and predictions do not change.
  m_keypointsImage->UpdateHostFromDevice();
  m_predictionsImage->UpdateHostFromDevice();

  const Vector2i imgSize = m_keypointsImage->noDims;
  const Keypoint3DColour *keypoints = m_keypointsImage->GetData(MEMORYDEVICE_CUDA);
  const ScorePrediction *predictions = m_predictionsImage->GetData(MEMORYDEVICE_CUDA);

  CUDARNG *randomGenerators = m_randomGenerators->GetData(MEMORYDEVICE_CUDA);
  PoseCandidate *poseCandidates = m_poseCandidates->GetData(MEMORYDEVICE_CUDA);
  int *nbPoseCandidates_device = m_nbPoseCandidates_device->GetData(MEMORYDEVICE_CUDA);

  dim3 blockSize(32);
  dim3 gridSize((m_nbMaxPoseCandidates + blockSize.x - 1) / blockSize.x);

  // Reset number of candidates (device only, the host number will be updated later, when we are done generating).
  ORcudaSafeCall(cudaMemsetAsync(nbPoseCandidates_device, 0, sizeof(int)));

  ck_preemptive_ransac_generate_pose_candidates<<<gridSize, blockSize>>>(keypoints,
                                                                         predictions,
                                                                         imgSize,
                                                                         randomGenerators,
                                                                         poseCandidates,
                                                                         nbPoseCandidates_device,
                                                                         m_nbMaxPoseCandidates,
                                                                         m_useAllModesPerLeafInPoseHypothesisGeneration,
                                                                         m_checkMinDistanceBetweenSampledModes,
                                                                         m_minSquaredDistanceBetweenSampledModes,
                                                                         m_checkRigidTransformationConstraint,
                                                                         m_translationErrorMaxForCorrectPose);
  ORcudaKernelCheck;

  // Need to make the data available to the host (for Kabsch).
  m_poseCandidates->dataSize = m_nbPoseCandidates_device->GetElement(0, MEMORYDEVICE_CUDA);
  m_poseCandidates->UpdateHostFromDevice();

  // Now perform kabsch on all candidates.
  compute_candidate_poses_kabsch();

  // Make the computed poses available to device.
  m_poseCandidates->UpdateDeviceFromHost();
}

void PreemptiveRansac_CUDA::sample_inlier_candidates(bool useMask)
{
  const Vector2i imgSize = m_keypointsImage->noDims;
  const Keypoint3DColour *keypointsData = m_keypointsImage->GetData(MEMORYDEVICE_CUDA);
  const ScorePrediction *predictionsData = m_predictionsImage->GetData(MEMORYDEVICE_CUDA);

  int *inlierMaskData = m_inliersMaskImage->GetData(MEMORYDEVICE_CUDA);
  int *inlierIndicesData = m_inliersIndicesBlock->GetData(MEMORYDEVICE_CUDA);
  int *nbInlier_device = m_nbSampledInliers_device->GetData(MEMORYDEVICE_CUDA);
  CUDARNG *randomGenerators = m_randomGenerators->GetData(MEMORYDEVICE_CUDA);

  // Only if the number of inliers (host side) is zero, we reset the device number.
  // The assumption is that the number on device memory will remain in sync with the host
  // since only this method is allowed to modify it.
  if (m_inliersIndicesBlock->dataSize == 0)
  {
    ORcudaSafeCall(cudaMemsetAsync(nbInlier_device, 0, sizeof(int)));
  }

  dim3 blockSize(128);
  dim3 gridSize((m_batchSizeRansac + blockSize.x - 1) / blockSize.x);

  if (useMask)
  {
    ck_preemptive_ransac_sample_inliers<true><<<gridSize, blockSize>>>(keypointsData,
                                                                       predictionsData,
                                                                       imgSize,
                                                                       randomGenerators,
                                                                       inlierIndicesData,
                                                                       nbInlier_device,
                                                                       m_batchSizeRansac,
                                                                       inlierMaskData);
    ORcudaKernelCheck;
  }
  else
  {
    ck_preemptive_ransac_sample_inliers<false><<<gridSize, blockSize>>>(keypointsData,
                                                                        predictionsData,
                                                                        imgSize,
                                                                        randomGenerators,
                                                                        inlierIndicesData,
                                                                        nbInlier_device,
                                                                        m_batchSizeRansac);
    ORcudaKernelCheck;
  }

  // Update the number of inliers
  m_inliersIndicesBlock->dataSize = static_cast<size_t>(m_nbSampledInliers_device->GetElement(0, MEMORYDEVICE_CUDA));
}

void PreemptiveRansac_CUDA::update_candidate_poses()
{
  // The pose update is currently implemented by the base class, need to copy the relevant data to host memory.
  m_inliersIndicesBlock->UpdateHostFromDevice();
  m_poseCandidates->UpdateHostFromDevice();

  PreemptiveRansac::update_candidate_poses();

  // The copy the updated poses back to the device.
  m_poseCandidates->UpdateDeviceFromHost();
}

//#################### PRIVATE MEMBER FUNCTIONS ####################

void PreemptiveRansac_CUDA::init_random()
{
  CUDARNG *randomGenerators = m_randomGenerators->GetData(MEMORYDEVICE_CUDA);

  // Initialize random states
  dim3 blockSize(256);
  dim3 gridSize((m_nbMaxPoseCandidates + blockSize.x - 1) / blockSize.x);

  ck_preemptive_ransac_init_random_generators<<<gridSize, blockSize>>>(
      randomGenerators, m_nbMaxPoseCandidates, m_rngSeed);
}

} // namespace grove