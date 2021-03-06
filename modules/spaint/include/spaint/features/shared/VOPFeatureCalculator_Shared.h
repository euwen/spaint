/**
 * spaint: VOPFeatureCalculator_Shared.h
 * Copyright (c) Torr Vision Group, University of Oxford, 2015. All rights reserved.
 */

#ifndef H_SPAINT_VOPFEATURECALCULATOR_SHARED
#define H_SPAINT_VOPFEATURECALCULATOR_SHARED

#include <itmx/util/ColourConversion_Shared.h>

#include "../../util/SpaintVoxel.h"

namespace spaint {

/**
 * \brief Converts the RGB patch for the specified voxel to the CIELab colour space.
 *
 * The patches are stored as the patch segments of the feature descriptors for the various voxels.
 *
 * \param voxelLocationIndex  The index of the voxel whose patch is to be converted.
 * \param featureCount        The number of features in a feature descriptor for a voxel.
 * \param features            The feature descriptors for the various voxels (stored sequentially).
 */
_CPU_AND_GPU_CODE_
inline void convert_patch_to_lab(int voxelLocationIndex, size_t featureCount, float *features)
{
  // Convert each RGB colour in the patch segment of the voxel's feature vector to the CIELab colour space.
  for(size_t i = voxelLocationIndex * featureCount, end = i + featureCount - 4; i != end; i += 3)
  {
    Vector3f rgb(features[i] / 255.0f, features[i+1] / 255.0f, features[i+2] / 255.0f);
    Vector3f lab = itmx::convert_rgb_to_lab(rgb);

    features[i] = lab.x;
    features[i+1] = lab.y;
    features[i+2] = lab.z;
  }
}

/**
 * \brief Computes a histogram of oriented gradients from a patch of intensity values.
 *
 * Note that each thread handles an individual pixel within a patch. On the GPU, there is a
 * thread block per patch, and we store the histograms in shared memory for efficiency.
 *
 * \param tid             The thread ID.
 * \param patchSize       The side length of a VOP patch (must be odd).
 * \param intensityPatch  The patch of intensity values from which to calculate the histogram.
 * \param binCount        The number of bins into which to quantize the gradient orientations.
 * \param histogram       The location into which to write the histogram for the patch.
 */
_CPU_AND_GPU_CODE_
inline void compute_histogram_for_patch(int tid, size_t patchSize, const float *intensityPatch, size_t binCount, float *histogram)
{
  // Compute the index and (x,y) coordinates of the pixel we're processing within the current patch.
  const int indexInPatch = tid % (patchSize * patchSize);
  const size_t y = indexInPatch / patchSize;
  const size_t x = indexInPatch % patchSize;

  // Provided we're within the boundaries of the patch and can safely compute a gradient:
  if(x != 0 && y != 0 && x != patchSize - 1 && y != patchSize - 1)
  {
    // Compute the x and y derivatives.
    float xDeriv = intensityPatch[indexInPatch + 1] - intensityPatch[indexInPatch - 1];
    float yDeriv = intensityPatch[indexInPatch + patchSize] - intensityPatch[indexInPatch - patchSize];

    // Compute the magnitude.
    float mag = static_cast<float>(sqrt(xDeriv * xDeriv + yDeriv * yDeriv));

    // Compute the orientation.
    double ori = atan2(yDeriv, xDeriv) + 2 * M_PI;

    // Quantize the orientation and update the histogram.
    int bin = static_cast<int>(binCount * ori / (2 * M_PI)) % binCount;

#if defined(__CUDACC__) && defined(__CUDA_ARCH__)
    atomicAdd(&histogram[bin], mag);
#else
  #ifdef WITH_OPENMP
    #pragma omp atomic
  #endif
    histogram[bin] += mag;
#endif
  }
}

/**
 * \brief Computes a patch of intensity values from an RGB patch.
 *
 * The RGB patches are stored as the patch segments of the feature descriptors for the various voxels.
 * Each thread processes one pixel of a patch. On the GPU, there is a thread block per patch, and the
 * intensity values are stored in shared memory for efficiency.
 *
 * \param tid             The thread ID.
 * \param features        The feature descriptors for the various voxels (stored sequentially).
 * \param featureCount    The number of features in a feature descriptor for a voxel.
 * \param patchSize       The side length of a VOP patch (must be odd).
 * \param intensityPatch  The location into which to write the intensity values for the patch.
 */
_CPU_AND_GPU_CODE_
inline void compute_intensities_for_patch(int tid, const float *features, int featureCount, int patchSize, float *intensityPatch)
{
  int patchArea = patchSize * patchSize;
  int voxelLocationIndex = tid / patchArea;
  int indexInPatch = tid % patchArea;

  const float *rgbPatch = features + voxelLocationIndex * featureCount;
  int pixelOffset = indexInPatch * 3;
  float r = rgbPatch[pixelOffset];
  float g = rgbPatch[pixelOffset + 1];
  float b = rgbPatch[pixelOffset + 2];

  intensityPatch[indexInPatch] = itmx::convert_rgb_to_grey(r, g, b);
}

/**
 * \brief Writes the height of the specified voxel into the corresponding feature vector for use as an extra feature.
 *
 * \param voxelLocationIndex  The index of the voxel whose height should be written.
 * \param voxelLocations      The locations of the voxels for which we are calculating features.
 * \param featureCount        The number of features in a feature descriptor for a voxel.
 * \param features            The feature descriptors for the various voxels (stored sequentially).
 */
_CPU_AND_GPU_CODE_
inline void fill_in_height(int voxelLocationIndex, const Vector3s *voxelLocations, size_t featureCount, float *features)
{
  features[(voxelLocationIndex + 1) * featureCount - 1] = voxelLocations[voxelLocationIndex].y;
}

/**
 * \brief Generates a unit vector that is perpendicular to the specified plane normal.
 *
 * The vector generated will be the normalised cross product of the specified plane normal and another vector
 * that is non-parallel to the normal. This non-parallel vector will be the up vector (0,0,1), unless that is
 * parallel to the normal, in which case (1,0,0) will be used instead.
 *
 * \param n The normal of the plane in which we want to generate the unit vector.
 * \return  The unit coplanar vector v as specified, satisfying v.dot(n) == 0.
 */
_CPU_AND_GPU_CODE_
inline Vector3f generate_arbitrary_coplanar_unit_vector(const Vector3f& n)
{
  Vector3f up(0.0f, 0.0f, 1.0f);
  if(fabs(n.x) < 1e-3f && fabs(n.y) < 1e-3f)
  {
    // Special Case: n is too close to the vertical and hence n x up is roughly equal to (0,0,0).
    // Use (1,0,0) instead of up and apply the same method as in the else clause.
    up = Vector3f(1.0f, 0.0f, 0.0f);
    return normalize(cross(n, Vector3f(1.0f, 0.0f, 0.0f)));
  }
  else
  {
    // The normalized cross product of n and up satisfies the requirements of being
    // unit length and perpendicular to n (since we dealt with the special case where
    // n x up is zero, in all other cases it must be non-zero and we can normalize it
    // to give us a unit vector).
    return normalize(cross(n, up));
  }
}

/**
 * \brief Generates an (x,y) coordinate system in the tangent plane of the specified voxel.
 *
 * \param voxelLocationIndex  The index of the voxel for which to generate a coordinate system.
 * \param surfaceNormals      The surface normals at the voxel locations.
 * \param xAxes               The array in which to store the generated x axis for the voxel.
 * \param yAxes               The array in which to store the generated y axis for the voxel.
 */
_CPU_AND_GPU_CODE_
inline void generate_coordinate_system(int voxelLocationIndex, const Vector3f *surfaceNormals, Vector3f *xAxes, Vector3f *yAxes)
{
  Vector3f n = surfaceNormals[voxelLocationIndex];
  Vector3f xAxis = generate_arbitrary_coplanar_unit_vector(n);
  xAxes[voxelLocationIndex] = xAxis;
  yAxes[voxelLocationIndex] = cross(xAxis, n);
}

/**
 * \brief Generates an RGB patch for the specified voxel by sampling from a regularly-spaced grid around it in its tangent plane.
 *
 * The RGB patches will be stored as the patch segments of the feature descriptors for the various voxels.
 *
 * \param voxelLocationIndex  The index of the voxel for which to generate an RGB patch.
 * \param voxelLocations      The locations of the voxels for which to generate RGB patches.
 * \param xAxes               The x axes of the coordinate systems in the tangent planes to the surfaces at the voxel locations.
 * \param yAxes               The y axes of the coordinate systems in the tangent planes to the surfaces at the voxel locations.
 * \param voxelData           The scene's voxel data.
 * \param indexData           The scene's index data.
 * \param patchSize           The side length of a VOP patch (must be odd).
 * \param patchSpacing        The spacing in the scene (in voxels) between individual pixels in a patch.
 * \param featureCount        The number of features in a feature descriptor for a voxel.
 * \param features            The feature descriptors for the various voxels (stored sequentially).
 */
_CPU_AND_GPU_CODE_
inline void generate_rgb_patch(int voxelLocationIndex, const Vector3s *voxelLocations, const Vector3f *xAxes, const Vector3f *yAxes,
                               const SpaintVoxel *voxelData, const ITMVoxelIndex::IndexData *indexData, size_t patchSize, float patchSpacing,
                               size_t featureCount, float *features)
{
  // Get the location of the voxel at the centre of the patch.
  Vector3f centre = voxelLocations[voxelLocationIndex].toFloat();

  // Generate an RGB patch around the voxel on a patchSize * patchSize grid aligned with the voxel's x and y axes.
  int halfPatchSize = static_cast<int>(patchSize - 1) / 2;
  bool isFound;
  Vector3f xAxis = xAxes[voxelLocationIndex] * patchSpacing;
  Vector3f yAxis = yAxes[voxelLocationIndex] * patchSpacing;

  // For each pixel in the patch:
  size_t offset = voxelLocationIndex * featureCount;
  for(int y = -halfPatchSize; y <= halfPatchSize; ++y)
  {
    Vector3f yLoc = centre + static_cast<float>(y) * yAxis;
    for(int x = -halfPatchSize; x <= halfPatchSize; ++x)
    {
      // Compute the location of the pixel in world space.
      Vector3i loc = (yLoc + static_cast<float>(x) * xAxis).toIntRound();

      // If there is a voxel at that location, get its colour; otherwise, default to magenta.
      Vector3u clr(255, 0, 255);
      SpaintVoxel voxel = readVoxel(voxelData, indexData, loc, isFound);
      if(isFound) clr = VoxelColourReader<SpaintVoxel::hasColorInformation>::read(voxel);

      // Write the colour values into the relevant places in the features array.
      features[offset++] = clr.r;
      features[offset++] = clr.g;
      features[offset++] = clr.b;
    }
  }
}

/**
 * \brief Updates the coordinate system for a voxel to align it with the dominant orientation in the voxel's RGB patch.
 *
 * Because of the way in which the coordinate system update has been parallelised, there is a thread running for each
 * pixel in the voxel's patch. However, the coordinate system for the voxel only needs to be updated once. As a result,
 * we only perform the update in the thread of the first pixel in the patch.
 *
 * \param tid       The thread ID.
 * \param patchArea The area of a voxel's patch.
 * \param histogram The histogram of oriented intensity gradients for the patch.
 * \param binCount  The number of quantized orientation bins in the histogram.
 * \param xAxis     The xAxis for the voxel corresponding to the current patch.
 * \param yAxis     The yAxis for the voxel corresponding to the current patch.
 */
_CPU_AND_GPU_CODE_
inline void update_coordinate_system(int tid, int patchArea, const float *histogram, size_t binCount, Vector3f *xAxis, Vector3f *yAxis)
{
  if(tid % patchArea == 0)
  {
    // Calculate the dominant orientation for the voxel.
    size_t dominantBin = 0;
    double highestBinValue = 0;
    for(size_t binIndex = 0; binIndex < binCount; ++binIndex)
    {
      double binValue = histogram[binIndex];
      if(binValue >= highestBinValue)
      {
        highestBinValue = binValue;
        dominantBin = binIndex;
      }
    }

    float binAngle = static_cast<float>(2 * M_PI) / binCount;
    float dominantOrientation = dominantBin * binAngle;

    // Rotate the existing axes to be aligned with the dominant orientation.
    float c = cos(dominantOrientation);
    float s = sin(dominantOrientation);

    Vector3f xAxisCopy = *xAxis;
    Vector3f yAxisCopy = *yAxis;

    *xAxis = c * xAxisCopy + s * yAxisCopy;
    *yAxis = c * yAxisCopy - s * xAxisCopy;
  }
}

/**
 * \brief Calculates the surface normal for the specified voxel and writes it into the surface normals array and the features array.
 *
 * \param voxelLocationIndex  The index of the voxel whose surface normal is to be written.
 * \param voxelLocations      The locations of the voxels for which to write surface normals.
 * \param voxelData           The scene's voxel data.
 * \param indexData           The scene's index data.
 * \param surfaceNormals      The surface normals at the voxel locations.
 * \param featureCount        The number of features in a feature descriptor for a voxel.
 * \param features            The features for the various voxels (packed sequentially).
 */
_CPU_AND_GPU_CODE_
inline void write_surface_normal(int voxelLocationIndex, const Vector3s *voxelLocations, const SpaintVoxel *voxelData, const ITMVoxelIndex::IndexData *indexData,
                                 Vector3f *surfaceNormals, size_t featureCount, float *features)
{
  // Compute the voxel's surface normal.
  Vector3f n = computeSingleNormalFromSDF(voxelData, indexData, voxelLocations[voxelLocationIndex].toFloat());

  // Write the normal into the surface normals array.
  surfaceNormals[voxelLocationIndex] = n;

  // Write the normal into the normal segment of the feature vector for the voxel.
  float *normalFeaturesForVoxel = features + (voxelLocationIndex + 1) * featureCount - 4;
  *normalFeaturesForVoxel++ = n.x;
  *normalFeaturesForVoxel++ = n.y;
  *normalFeaturesForVoxel++ = n.z;
}

}

#endif
