/**
 * spaint: ArUcoFiducialDetector.h
 * Copyright (c) Torr Vision Group, University of Oxford, 2016. All rights reserved.
 */

#ifndef H_SPAINT_ARUCOFIDUCIALDETECTOR
#define H_SPAINT_ARUCOFIDUCIALDETECTOR

#include <boost/optional.hpp>

#include <opencv2/opencv.hpp>

#include <ITMLib/Engines/Visualisation/Interface/ITMVisualisationEngine.h>

#include <itmx/picking/interface/Picker.h>

#include "FiducialDetector.h"
#include "../util/SpaintVoxelScene.h"

namespace spaint {

/**
 * \brief An instance of this class can be used to detect ArUco fiducials in a 3D scene.
 */
class ArUcoFiducialDetector : public FiducialDetector
{
  //#################### TYPEDEFS ####################
private:
  typedef boost::shared_ptr<const ITMLib::ITMVisualisationEngine<SpaintVoxel,ITMVoxelIndex> > VoxelVisualisationEngine_CPtr;

  //#################### ENUMERATIONS ####################
public:
  /**
   * \brief The values of this enumeration denote the different fiducial pose estimation modes that are supported.
   */
  enum PoseEstimationMode
  {
    /** Estimate the poses of the fiducials from the live colour image. */
    PEM_COLOUR,

    /** Estimate the poses of the fiducials from the live depth image. */
    PEM_DEPTH,

    /** Estimate the poses of the fiducials from a depth raycast of the scene. */
    PEM_RAYCAST
  };

  //#################### PRIVATE VARIABLES ####################
private:
  /** The picker used when estimating poses from the scene raycast. */
  mutable itmx::Picker_CPtr m_picker;

  /** The mode to use when estimating the poses of the fiducials. */
  PoseEstimationMode m_poseEstimationMode;

  /** The render state to use when rendering the scene raycast. */
  mutable VoxelRenderState_Ptr m_renderState;

  /** The 3D voxel scene. */
  SpaintVoxelScene_CPtr m_scene;

  /** The settings to use for InfiniTAM. */
  Settings_CPtr m_settings;

  /** The InfiniTAM engine to use for rendering a voxel scene. */
  VoxelVisualisationEngine_CPtr m_voxelVisualisationEngine;

  //#################### CONSTRUCTORS ####################
public:
  /**
   * \brief Constructs an ArUco fiducial detector.
   *
   * \param scene               The 3D voxel scene.
   * \param settings            The settings to use for InfiniTAM.
   * \param poseEstimationMode  The mode to use when estimating the poses of the fiducials.
   */
  ArUcoFiducialDetector(const SpaintVoxelScene_CPtr& scene, const Settings_CPtr& settings, PoseEstimationMode poseEstimationMode);

  //#################### PUBLIC MEMBER FUNCTIONS ####################
public:
  /** Override */
  virtual std::map<std::string,FiducialMeasurement> detect_fiducials(const View_CPtr& view, const ORUtils::SE3Pose& depthPose) const;

  //#################### PRIVATE MEMBER FUNCTIONS ####################
private:
  /**
   * \brief Constructs a set of fiducial measurements by directly estimating poses for the fiducials from the live colour image.
   *
   * \note  The poses are estimated using the OpenCV ArUco library, and are much less accurate than the poses we can get using
   *        either the live depth image or a raycast of the scene. The sole advantage of this approach is that it can produce
   *        poses when neither of those two sources of information are available.
   *
   * \param ids       The IDs of the fiducials that have been detected in the live colour image.
   * \param corners   The corners of the fiducials that have been detected in the live colour image.
   * \param view      The view of the scene containing the live images.
   * \param depthPose The current estimate of the depth camera pose (used to map between eye space and world space).
   * \return          The constructed set of fiducial measurements.
   */
  std::vector<boost::optional<FiducialMeasurement> >
  construct_measurements_from_colour(const std::vector<int>& ids, const std::vector<std::vector<cv::Point2f> >& corners,
                                     const View_CPtr& view, const ORUtils::SE3Pose& depthPose) const;

  /**
   * \brief Constructs a set of fiducial measurements by back-projecting the detected fiducial corners in the live colour image
   *        into 3D using depth values from the live depth image, and then using the back-projected corners to determine poses
   *        for the fiducials in both eye and world space.
   *
   * \param ids       The IDs of the fiducials that have been detected in the live colour image.
   * \param corners   The corners of the fiducials that have been detected in the live colour image.
   * \param view      The view of the scene containing the live images.
   * \param depthPose The current estimate of the depth camera pose (used to map between eye space and world space).
   * \return          The constructed set of fiducial measurements.
   */
  std::vector<boost::optional<FiducialMeasurement> >
  construct_measurements_from_depth(const std::vector<int>& ids, const std::vector<std::vector<cv::Point2f> >& corners,
                                    const View_CPtr& view, const ORUtils::SE3Pose& depthPose) const;

  /**
   * \brief Constructs a set of fiducial measurements by looking up in a raycast of the scene the 3D points in world space
   *        that correspond to the detected fiducial corners in the live colour image, and then using these 3D points to
   *        determine poses for the fiducials in both world and eye space.
   *
   * \param ids         The IDs of the fiducials that have been detected in the live colour image.
   * \param corners     The corners of the fiducials that have been detected in the live colour image.
   * \param view        The view of the scene containing the live images.
   * \param depthPose   The current estimate of the depth camera pose (used to map between world space and eye space).
   * \return            The constructed set of fiducial measurements.
   */
  std::vector<boost::optional<FiducialMeasurement> >
  construct_measurements_from_raycast(const std::vector<int>& ids, const std::vector<std::vector<cv::Point2f> >& corners,
                                      const View_CPtr& view, const ORUtils::SE3Pose& depthPose) const;

  /**
   * \brief Tries to determine the 3D point in eye space that corresponds to a fiducial corner in the live colour image
   *        by back-projecting into 3D using the depth value from the live depth image.
   *
   * \param corner  The fiducial corner in the live colour image.
   * \param view    The view of the scene containing the live images.
   * \return        The 3D point in eye space corresponding to the fiducial corner (if any), or boost::none otherwise.
   */
  boost::optional<Vector3f> pick_corner_from_depth(const cv::Point2f& corner, const View_CPtr& view) const;

  /**
   * \brief Tries to determine the 3D point in world space that corresponds to a fiducial corner in the live colour image
   *        by looking it up in a raycast of the scene from the pose of the colour camera.
   *
   * \param corner  The fiducial corner in the live colour image.
   * \return        The 3D point in world space corresponding to the fiducial corner (if any), or boost::none otherwise.
   */
  boost::optional<Vector3f> pick_corner_from_raycast(const cv::Point2f& corner) const;
};

}

#endif
