/**
 * itmx: MappingClient.cpp
 * Copyright (c) Torr Vision Group, University of Oxford, 2017. All rights reserved.
 */

#include "remotemapping/MappingClient.h"

#include <stdexcept>

#include <tvgutil/boost/WrappedAsio.h>
#include <tvgutil/net/AckMessage.h>
using boost::asio::ip::tcp;
using namespace tvgutil;

#include "remotemapping/InteractionTypeMessage.h"
#include "remotemapping/RenderingRequestMessage.h"

namespace itmx {

//#################### CONSTRUCTORS ####################

MappingClient::MappingClient(const std::string& host, const std::string& port, pooled_queue::PoolEmptyStrategy poolEmptyStrategy)
: m_frameMessageQueue(poolEmptyStrategy), m_stream(host, port)
{
  if(!m_stream) throw std::runtime_error("Error: Could not connect to server");
}

//#################### PUBLIC MEMBER FUNCTIONS ####################

MappingClient::RGBDFrameMessageQueue::PushHandler_Ptr MappingClient::begin_push_frame_message()
{
  return m_frameMessageQueue.begin_push();
}

ORUChar4Image_CPtr MappingClient::get_remote_image() const
{
  AckMessage ackMsg;
  InteractionTypeMessage interactionTypeMsg(IT_HASRENDEREDIMAGE);

  boost::lock_guard<boost::mutex> lock(m_interactionMutex);

  // Ask the server whether it has ever rendered an RGB-D image for this client.
  if(m_stream.write(interactionTypeMsg.get_data_ptr(), interactionTypeMsg.get_size()))
  {
    SimpleMessage<bool> flag;
    if(m_stream.read(flag.get_data_ptr(), flag.get_size()) && m_stream.write(ackMsg.get_data_ptr(), ackMsg.get_size()) && flag.extract_value())
    {
      // If it has, ask it to send across the RGB-D image it has rendered for this client.
      interactionTypeMsg.set_value(IT_GETRENDEREDIMAGE);
      if(m_stream.write(interactionTypeMsg.get_data_ptr(), interactionTypeMsg.get_size()))
      {
        // Read the compressed RGB-D frame it sends across.
        CompressedRGBDFrameHeaderMessage headerMsg;
        if(m_stream.read(headerMsg.get_data_ptr(), headerMsg.get_size()))
        {
          CompressedRGBDFrameMessage frameMsg(headerMsg);
          if(m_stream.read(frameMsg.get_data_ptr(), frameMsg.get_size()))
          {
            // Send an acknowledgement that we've received the frame.
            m_stream.write(ackMsg.get_data_ptr(), ackMsg.get_size());

            // Uncompress the frame.
            // FIXME: Avoid creating a new uncompressed frame every time.
            const Vector2i rgbImageSize = headerMsg.extract_rgb_image_size();
            const Vector2i depthImageSize = headerMsg.extract_depth_image_size();
            RGBDFrameMessage uncompressedFrameMsg(rgbImageSize, depthImageSize);
            m_frameCompressor->uncompress_rgbd_frame(frameMsg, uncompressedFrameMsg);

            // Extract the colour image from the frame and use it to update the remote image for this client.
            if(!m_remoteImage) m_remoteImage.reset(new ORUChar4Image(rgbImageSize, true, false));
            m_remoteImage->ChangeDims(rgbImageSize);
            uncompressedFrameMsg.extract_rgb_image(m_remoteImage.get());

            return m_remoteImage;
          }
        }
      }
    }
  }

  // If anything went wrong, return a blank image.
  return ORUChar4Image_CPtr();
}

void MappingClient::send_calibration_message(const RGBDCalibrationMessage& msg)
{
  bool connectionOk = true;

  // Send the message to the server.
  connectionOk = connectionOk && m_stream.write(msg.get_data_ptr(), msg.get_size());

  // Wait for an acknowledgement (note that this is blocking, unless the connection fails).
  AckMessage ackMsg;
  connectionOk = connectionOk && m_stream.read(ackMsg.get_data_ptr(), ackMsg.get_size());

  // Throw if the message was not successfully sent and acknowledged.
  if(!connectionOk) throw std::runtime_error("Error: Failed to send calibration message");

  // Initialise the frame message queue.
  const int capacity = 1;
  const ITMLib::ITMRGBDCalib calib = msg.extract_calib();
  const Vector2i rgbImageSize = calib.intrinsics_rgb.imgSize;
  const Vector2i depthImageSize = calib.intrinsics_d.imgSize;
  m_frameMessageQueue.initialise(capacity, boost::bind(&RGBDFrameMessage::make, rgbImageSize, depthImageSize));

  // Set up the RGB-D frame compressor.
  m_frameCompressor.reset(new RGBDFrameCompressor(rgbImageSize, depthImageSize, msg.extract_rgb_compression_type(), msg.extract_depth_compression_type()));

  // Start the message sender thread.
  boost::thread messageSender(&MappingClient::run_message_sender, this);
}

void MappingClient::update_rendering_request(const Vector2i& imgSize, const ORUtils::SE3Pose& pose, int visualisationType)
{
  AckMessage ackMsg;
  InteractionTypeMessage interactionTypeMsg(IT_UPDATERENDERINGREQUEST);

  RenderingRequestMessage requestMsg;
  requestMsg.set_image_size(imgSize);
  requestMsg.set_pose(pose);
  requestMsg.set_visualisation_type(visualisationType);

  boost::lock_guard<boost::mutex> lock(m_interactionMutex);

  // First send the interaction type message, then send the rendering request message,
  // then wait for an acknowledgement from the server. We chain all of these with &&
  // so as to early out in case of failure.
  m_stream.write(interactionTypeMsg.get_data_ptr(), interactionTypeMsg.get_size()) &&
  m_stream.write(requestMsg.get_data_ptr(), requestMsg.get_size()) && 
  m_stream.read(ackMsg.get_data_ptr(), ackMsg.get_size());
}

//#################### PRIVATE MEMBER FUNCTIONS ####################

void MappingClient::run_message_sender()
{
  AckMessage ackMsg;
  CompressedRGBDFrameHeaderMessage headerMsg;
  CompressedRGBDFrameMessage frameMsg(headerMsg);
  InteractionTypeMessage interactionTypeMsg(IT_SENDFRAME);

  bool connectionOk = true;

  while(connectionOk)
  {
    // Read the first frame message from the queue (this will block until a message is available).
    RGBDFrameMessage_Ptr msg = m_frameMessageQueue.peek();

    // Compress the frame. The compressed frame is split into two messages - a header message,
    // which tells the server how large a frame to expect, and a separate message containing
    // the actual frame data.
    m_frameCompressor->compress_rgbd_frame(*msg, headerMsg, frameMsg);

    {
      boost::lock_guard<boost::mutex> lock(m_interactionMutex);

      // First send the interaction type message, then send the frame header message, then send
      // the frame message itself, then wait for an acknowledgement from the server. We chain
      // all of these with && so as to early out in case of failure.
      connectionOk = connectionOk
        && m_stream.write(interactionTypeMsg.get_data_ptr(), interactionTypeMsg.get_size())
        && m_stream.write(headerMsg.get_data_ptr(), headerMsg.get_size())
        && m_stream.write(frameMsg.get_data_ptr(), frameMsg.get_size())
        && m_stream.read(ackMsg.get_data_ptr(), ackMsg.get_size());
    }

    // Remove the frame message that we have just sent from the queue.
    m_frameMessageQueue.pop();
  }
}

}
