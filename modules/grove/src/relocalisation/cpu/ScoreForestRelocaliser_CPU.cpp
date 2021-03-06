/**
 * grove: ScoreForestRelocaliser_CPU.cpp
 * Copyright (c) Torr Vision Group, University of Oxford, 2017. All rights reserved.
 */

#include "relocalisation/cpu/ScoreForestRelocaliser_CPU.h"
using namespace ORUtils;
using namespace tvgutil;

#include "relocalisation/shared/ScoreForestRelocaliser_Shared.h"

namespace grove {

//#################### CONSTRUCTORS ####################

ScoreForestRelocaliser_CPU::ScoreForestRelocaliser_CPU(const SettingsContainer_CPtr& settings, const std::string& settingsNamespace)
: ScoreForestRelocaliser(settings, settingsNamespace, DEVICE_CPU)
{}

//#################### PROTECTED MEMBER FUNCTIONS ####################

void ScoreForestRelocaliser_CPU::merge_predictions_for_keypoints(const LeafIndicesImage_CPtr& leafIndices, ScorePredictionsImage_Ptr& outputPredictions) const
{
  const Vector2i imgSize = leafIndices->noDims;

  // Make sure that the output predictions image has the right size (this is a no-op after the first time).
  outputPredictions->ChangeDims(imgSize);

  const LeafIndices *leafIndicesPtr = leafIndices->GetData(MEMORYDEVICE_CPU);
  ScorePrediction *outputPredictionsPtr = outputPredictions->GetData(MEMORYDEVICE_CPU);
  const ScorePrediction *predictionsBlockPtr = m_relocaliserState->predictionsBlock->GetData(MEMORYDEVICE_CPU);

#ifdef WITH_OPENMP
  #pragma omp parallel for
#endif
  for(int y = 0; y < imgSize.y; ++y)
  {
    for(int x = 0; x < imgSize.x; ++x)
    {
      merge_predictions_for_keypoint(x, y, leafIndicesPtr, predictionsBlockPtr, imgSize, m_maxClusterCount, outputPredictionsPtr);
    }
  }
}

}
