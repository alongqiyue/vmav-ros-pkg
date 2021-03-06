#include <boost/algorithm/string.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include <boost/program_options.hpp>
#include <cv_bridge/cv_bridge.h>
#include <fstream>
#include <iomanip>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <px_comm/CameraInfo.h>
#include <px_comm/SetCameraInfo.h>
#include <ros/ros.h>
#include <rosbag/bag.h>
#include <rosbag/view.h>

#include "camera_models/CameraFactory.h"
#include "camera_systems/CameraSystem.h"
#include "cauldron/EigenUtils.h"
#include "self_multicam_calibration/SelfMultiCamCalibration.h"

#define N_CAMERAS 4

bool
parseTokenFromString(std::string& s, std::string& token, size_t& pos)
{
    if (s.empty())
    {
        return false;
    }

	pos = s.find(" ");
	token = s.substr(0, pos);
	s.erase(0, pos + 1);

	return true;
}

bool
parseConfigFile(const std::string& configFilename,
                std::vector<std::vector<std::string> >& cameraNs,
                std::string& imuTopicName)
{
    bool hasStereo = false;
    bool hasIMU = false;

    cameraNs.clear();
    imuTopicName.clear();

    std::ifstream ifs(configFilename.c_str());

    if (!ifs.is_open())
    {
        return false;
    }

    while (ifs.good())
    {
        std::string s;
        std::getline(ifs, s);

        if (s.empty())
        {
        	continue;
        }

        size_t pos = 0;
        std::string token;

        if (!parseTokenFromString(s, token, pos))
        {
        	continue;
        }

        if (boost::iequals(token, "stereo"))
        {
            std::string leftTopicName;
            std::string rightTopicName;

            if (!parseTokenFromString(s, leftTopicName, pos))
            {
                ROS_ERROR("The stereo camera sensor is improperly defined.");
                return false;
            }

            if (!parseTokenFromString(s, rightTopicName, pos))
            {
                ROS_ERROR("The stereo camera sensor is improperly defined.");
                return false;
            }

            cameraNs.push_back(std::vector<std::string>());
            cameraNs.back().push_back(leftTopicName);
            cameraNs.back().push_back(rightTopicName);

            hasStereo = true;
        }
        else if (boost::iequals(token, "mono"))
        {
            std::string topicName;

            if (!parseTokenFromString(s, topicName, pos))
            {
                ROS_ERROR("The mono camera sensor is improperly defined.");
                return false;
            }

            cameraNs.push_back(std::vector<std::string>());
            cameraNs.back().push_back(topicName);
        }
        else if (boost::iequals(token, "imu"))
        {
            if (hasIMU)
            {
                ROS_ERROR("A duplicate definition was found for the imu sensor.");
                return false;
            }

            if (!parseTokenFromString(s, imuTopicName, pos))
            {
                ROS_ERROR("The IMU sensor is improperly defined.");
                return false;
            }

            hasIMU = true;
        }
        else
        {
            ROS_ERROR("Unknown sensor type: %s", token.c_str());
            return false;
        }
    }

    if (!hasStereo || !hasIMU)
    {
        return false;
    }

    return true;
}

void
voCallback(const sensor_msgs::ImageConstPtr& imageMsg0,
           const sensor_msgs::ImageConstPtr& imageMsg1,
           const sensor_msgs::ImageConstPtr& imageMsg2,
           const sensor_msgs::ImageConstPtr& imageMsg3,
           const std::list<sensor_msgs::ImuConstPtr>& imuBuffer,
           boost::shared_ptr<px::SelfMultiCamCalibration> sc)
{
    ros::Time stamp = imageMsg0->header.stamp;

    sensor_msgs::ImuConstPtr imuMsg;
    std::list<sensor_msgs::ImuConstPtr>::const_reverse_iterator rit = imuBuffer.rbegin();
    while (rit != imuBuffer.rend())
    {
        if ((*rit)->header.stamp == stamp)
        {
            imuMsg = *rit;
            break;
        }
        ++rit;
    }

    if (!imuMsg)
    {
        ROS_WARN("No IMU message with matching timestamp.");
        return;
    }

    std::vector<sensor_msgs::ImageConstPtr> imageMsgs;
    imageMsgs.push_back(imageMsg0);
    imageMsgs.push_back(imageMsg1);
    imageMsgs.push_back(imageMsg2);
    imageMsgs.push_back(imageMsg3);

    std::vector<cv::Mat> images(N_CAMERAS);
    try
    {
        for (int i = 0; i < N_CAMERAS; ++i)
        {
            cv_bridge::CvImageConstPtr cv_ptr = cv_bridge::toCvShare(imageMsgs.at(i));
            cv_ptr->image.copyTo(images.at(i));
        }
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }

    sc->processFrames(stamp, images, imuMsg);
}

template<class M>
class BagSubscriber: public message_filters::SimpleFilter<M>
{
public:
    void newMessage(const boost::shared_ptr<const M>& msg)
    {
        this->signalMessage(msg);
    }
};

int
main(int argc, char** argv)
{
    std::string bagFilename;
    std::string vocFilename;
    std::string configFilename;
    bool readIntermediateData = false;
    std::string chessboardDataDir;
    std::string outputDir;

    boost::program_options::options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("input,i", boost::program_options::value<std::string>(&bagFilename), "ROS bag filename.")
        ("voc", boost::program_options::value<std::string>(&vocFilename)->default_value("orb.yml.gz"), "Vocabulary filename.")
        ("config,c", boost::program_options::value<std::string>(&configFilename)->default_value("self_calib.cfg"), "Configuration file.")
        ("intermediate", boost::program_options::bool_switch(&readIntermediateData), "Read intermediate map data in lieu of VO.")
        ("chessboard-data", boost::program_options::value<std::string>(&chessboardDataDir), "Directory containing chessboard data files.")
        ("output,o", boost::program_options::value<std::string>(&outputDir)->default_value("calib"), "Output directory.")
        ;

    boost::program_options::variables_map vm;
    boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
    boost::program_options::notify(vm);

    if (vm.count("help"))
    {
        std::cout << desc << std::endl;
        return 1;
    }

    ros::init(argc, argv, "extrinsic_calibration");

    std::vector<std::vector<std::string> > cameraNs;
    std::string imuTopicName;

    if (!parseConfigFile(configFilename, cameraNs, imuTopicName))
    {
        ROS_ERROR("Failed to read configuration file %s", configFilename.c_str());
        return 1;
    }

    size_t nCams = 0;
    for (size_t i = 0; i < cameraNs.size(); ++i)
    {
        nCams += cameraNs.at(i).size();
    }

    px::CameraSystemPtr cameraSystem = boost::make_shared<px::CameraSystem>(nCams);

    rosbag::Bag bag;
    try
    {
        bag.open(bagFilename, rosbag::bagmode::Read);
    }
    catch (rosbag::BagIOException&)
    {
        ROS_ERROR("Unable to open bag file: %s", bagFilename.c_str());
        return 1;
    }

    std::vector<std::string> camInfoTopicNames;
    std::vector<std::string> camImageTopicNames;
    for (size_t i = 0; i < cameraNs.size(); ++i)
    {
        for (size_t j = 0; j < cameraNs.at(i).size(); ++j)
        {
            camInfoTopicNames.push_back(cameraNs.at(i).at(j) + "/camera_info");
            camImageTopicNames.push_back(cameraNs.at(i).at(j) + "/image_raw");
        }
    }

    std::vector<std::string> topics;
    topics.insert(topics.end(), camInfoTopicNames.begin(), camInfoTopicNames.end());
    topics.insert(topics.end(), camImageTopicNames.begin(), camImageTopicNames.end());
    topics.push_back(imuTopicName);

    rosbag::View view(bag, rosbag::TopicQuery(topics));

    ros::NodeHandle nh;

    boost::shared_ptr<px::SelfMultiCamCalibration> sc;
    px::SparseGraphPtr sparseGraph = boost::make_shared<px::SparseGraph>();

    std::vector<boost::shared_ptr<BagSubscriber<sensor_msgs::Image> > > imageSubs(nCams);
    for (size_t i = 0; i < imageSubs.size(); ++i)
    {
        imageSubs.at(i) = boost::make_shared<BagSubscriber<sensor_msgs::Image> >();
    }

    std::vector<px::CameraPtr> cameras(nCams);
    std::list<sensor_msgs::ImuConstPtr> imuBuffer;

    // for now, assume 4 cameras are present
    message_filters::TimeSynchronizer<sensor_msgs::Image,
                                      sensor_msgs::Image,
                                      sensor_msgs::Image,
                                      sensor_msgs::Image> sync(*imageSubs.at(0),
                                                               *imageSubs.at(1),
                                                               *imageSubs.at(2),
                                                               *imageSubs.at(3),
                                                               5);

    ros::Time calibStartTime = ros::Time::now();

    if (readIntermediateData)
    {
        if (!cameraSystem->readFromTextFile("int_camera_system_extrinsics.txt"))
        {
            ROS_ERROR("Failed to read intermediate camera extrinsic file.");
            return 1;
        }
    }
    else
    {
        ROS_INFO("Running visual odometry...");
    }

    std::vector<ros::Publisher> imagePubs(camImageTopicNames.size());
    for (int i = 0; i < camImageTopicNames.size(); ++i)
    {
        imagePubs.at(i) = nh.advertise<sensor_msgs::Image>(camImageTopicNames.at(i), 2);
    }

    boost::dynamic_bitset<> init(N_CAMERAS);
    BOOST_FOREACH(rosbag::MessageInstance const m, view)
    {
        if (!ros::ok())
        {
            break;
        }

        if (init.count() < N_CAMERAS)
        {
            for (int i = 0; i < N_CAMERAS; ++i)
            {
                if (m.getTopic() == camInfoTopicNames.at(i))
                {
                    if (init[i])
                    {
                        break;
                    }

                    px_comm::CameraInfoConstPtr cameraInfo = m.instantiate<px_comm::CameraInfo>();
                    if (cameraInfo)
                    {
                        cameras.at(i) = px::CameraFactory::instance()->generateCamera(cameraInfo);

                        if (!readIntermediateData)
                        {
                            cameraSystem->setGlobalCameraPose(i, cameraInfo->pose);
                        }

                        init[i] = 1;
                    }

                    break;
                }
            }

            if (init.count() == N_CAMERAS)
            {
                size_t mark = 0;
                for (size_t i = 0; i < cameraNs.size(); ++i)
                {
                    if (cameraNs.at(i).size() == 2)
                    {
                        Eigen::Matrix4d H = px::invertHomogeneousTransform(cameraSystem->getGlobalCameraPose(mark)) *
                                            cameraSystem->getGlobalCameraPose(mark + 1);

                        cameras.at(mark)->cameraType() = "stereo";
                        cameras.at(mark + 1)->cameraType() = "stereo";

                        cameraSystem->setGlobalCameraPose(mark, Eigen::Matrix4d::Identity());
                        cameraSystem->setGlobalCameraPose(mark + 1, H);

                        cameraSystem->setCamera(mark, cameras.at(mark));
                        cameraSystem->setCamera(mark + 1, cameras.at(mark + 1));
                    }
                    else
                    {
                        cameras.at(mark)->cameraType() = "mono";

                        cameraSystem->setGlobalCameraPose(mark, Eigen::Matrix4d::Identity());

                        cameraSystem->setCamera(mark, cameras.at(mark));
                    }

                    mark += cameraNs.at(i).size();
                }

                sc = boost::make_shared<px::SelfMultiCamCalibration>(boost::ref(nh),
                                                                     boost::ref(cameraSystem),
                                                                     boost::ref(sparseGraph));
                if (!sc->init("STAR", "ORB", "BruteForce-Hamming"))
                {
                    ROS_ERROR("Failed to initialize extrinsic calibration.");
                    return 1;
                }

                sync.registerCallback(boost::bind(&voCallback, _1, _2, _3, _4, boost::ref(imuBuffer), sc));

                ROS_INFO("Initialized extrinsic calibration.");
            }
        }
        else
        {
            if (readIntermediateData)
            {
                break;
            }

            for (int i = 0; i < N_CAMERAS; ++i)
            {
                if (m.getTopic() == camImageTopicNames.at(i))
                {
                    sensor_msgs::ImageConstPtr img = m.instantiate<sensor_msgs::Image>();
                    if (img)
                    {
                        imageSubs.at(i)->newMessage(img);
                    }
                    imagePubs.at(i).publish(img);
                }
            }

            if (m.getTopic() == imuTopicName)
            {
                sensor_msgs::ImuConstPtr imu = m.instantiate<sensor_msgs::Imu>();
                if (imu)
                {
                    imuBuffer.push_back(imu);
                    while (imuBuffer.size() > 50)
                    {
                        imuBuffer.pop_front();
                    }
                }
            }
        }
    }

    bag.close();

    if (!ros::ok())
    {
        ROS_ERROR("Aborted.");
        return 1;
    }

    ROS_INFO("Running extrinsic calibration...");

    sc->run(vocFilename, chessboardDataDir, readIntermediateData);

    ROS_INFO("Done!");

    unsigned int calibDuration = static_cast<unsigned int>((ros::Time::now() - calibStartTime).toSec());
    ROS_INFO("Calibration took %u m %u s.", calibDuration / 60, calibDuration % 60);

    cameraSystem->writeToDirectory(outputDir);
    ROS_INFO_STREAM("Wrote calibration files to "
                    << boost::filesystem::absolute(boost::filesystem::path(outputDir)).string());

    std::cout << std::fixed << std::setprecision(5);
    for (int i = 0; i < cameraSystem->cameraCount(); ++i)
    {
        ROS_INFO_STREAM(cameraSystem->getCamera(i)->cameraName() << std::endl
                        << cameraSystem->getGlobalCameraPose(i) << std::endl);
    }

    // send SetCameraInfo request
    for (size_t i = 0; i < camInfoTopicNames.size(); ++i)
    {
        ros::ServiceClient cameraInfoClient;
        cameraInfoClient = nh.serviceClient<px_comm::SetCameraInfo>(camInfoTopicNames.at(i));

        px_comm::CameraInfoPtr cameraInfo = boost::make_shared<px_comm::CameraInfo>();
        cameraSystem->getCamera(i)->writeParameters(cameraInfo);

        Eigen::Matrix4d H = cameraSystem->getGlobalCameraPose(i);
        Eigen::Quaterniond q(H.block<3,3>(0,0));

        cameraInfo->pose.orientation.w = q.w();
        cameraInfo->pose.orientation.x = q.x();
        cameraInfo->pose.orientation.y = q.y();
        cameraInfo->pose.orientation.z = q.z();

        cameraInfo->pose.position.x = H(0,3);
        cameraInfo->pose.position.y = H(1,3);
        cameraInfo->pose.position.z = H(2,3);

        px_comm::SetCameraInfo setCameraInfo;
        setCameraInfo.request.camera_info = *cameraInfo;

        if (cameraInfoClient.call(setCameraInfo))
        {
            ROS_INFO("Received reply to SetCameraInfo request for camera [%s].",
                     cameraInfo->camera_name.c_str());

            if (setCameraInfo.response.success == true)
            {
                ROS_INFO("SetCameraInfo request for camera [%s] was processed successfully: %s.",
                         cameraInfo->camera_name.c_str(),
                         setCameraInfo.response.status_message.c_str());
            }
            else
            {
                ROS_ERROR("SetCameraInfo request for camera [%s] was not processed: %s.",
                          cameraInfo->camera_name.c_str(),
                          setCameraInfo.response.status_message.c_str());
            }
        }
        else
        {
            ROS_ERROR("Did not receive reply to SetCameraInfo request for camera [%s].",
                      cameraInfo->camera_name.c_str());
        }
    }

    // write MAV poses to file
    sc->writePosesToTextFile("vmav_calib_poses.txt");

    return 0;
}

