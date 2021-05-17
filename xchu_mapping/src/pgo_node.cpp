//
// Created by xchu on 2021/5/12.
//
#include "xchu_mapping/pgo.h"

int main(int argc, char **argv) {
  ros::init(argc, argv, "pgo_node");
  ROS_INFO("\033[1;32m---->\033[0m XCHU PGO Node Started.");

  PGO pgo;
  std::thread loop_detection(&PGO::LoopClosure, &pgo); // 会换检测 2hz
  std::thread icp_calculation(&PGO::ICPRefine, &pgo); // 会还检测优化 1hz
  std::thread visualize_map(&PGO::MapVisualization, &pgo);

  ros::Rate rate(20);
  while (ros::ok()) {
    pgo.Run();
    ros::spinOnce();
    rate.sleep();
  }
  loop_detection.join();
  icp_calculation.join();
  visualize_map.join();

  ros::spin();
  return 0;
}

PGO::PGO() : nh("~") {
  InitParams();

  points_sub = nh.subscribe<sensor_msgs::PointCloud2>("/no_ground_points", 100, &PGO::PcCB, this);
  odom_sub = nh.subscribe<nav_msgs::Odometry>("/laser_odom_to_init", 100, &PGO::OdomCB, this);
  gps_sub = nh.subscribe<sensor_msgs::NavSatFix>("/kitti/oxts/gps/fix", 100, &PGO::GpsCB, this);

  // odom_pose_pub = nh.advertise<sensor_msgs::PointCloud2>("/odom_poses", 10);
  final_odom_pub = nh.advertise<nav_msgs::Odometry>("/final_odom", 100);
  map_pub = nh.advertise<sensor_msgs::PointCloud2>("/global_map", 100);
  pose_pub = nh.advertise<sensor_msgs::PointCloud2>("/key_poses", 1);  // key pose
  markers_pub = nh.advertise<visualization_msgs::MarkerArray>("/markers", 16);
  isc_pub = nh.advertise<sensor_msgs::Image>("/isc", 100);
}

void PGO::InitParams() {
  nh.param<std::string>("save_dir_", save_dir_, "/home/xchu/workspace/xchujwu_slam/src/xchu_mapping/pcd/");
  nh.param<int>("loop_method", loop_method, 2);

  keyframeMeterGap = 2.0;// pose assignment every k frames
  if (loop_method == 1) {
    scDistThres = 0.2;// pose assignment every k frames
    scManager.setSCdistThres(scDistThres);
  } else if (loop_method == 2) {
    //read parameter
    int sector_width = 60;
    int ring_height = 60;
    double max_dis = 40.0;
    iscGeneration.init_param(ring_height, sector_width, max_dis);
  }

  curr_frame_.reset(new pcl::PointCloud<PointT>());
  laserCloudMapAfterPGO.reset(new pcl::PointCloud<PointT>());
  laserCloudMapPGO.reset(new pcl::PointCloud<PointT>());
  keyposes_cloud_.reset(new pcl::PointCloud<PointT>());
  keyframePoints.reset(new pcl::PointCloud<PointT>());
  kdtreeHistoryKeyPoses.reset(new pcl::KdTreeFLANN<PointT>());

  float filter_size = 0.5;
  downSizeFilterScancontext.setLeafSize(filter_size, filter_size, filter_size);
  downSizeFilterICP.setLeafSize(filter_size, filter_size, filter_size);
  downSizePublishCloud.setLeafSize(1.0, 1.0, 1.0);
  downSizeFilterMapPGO.setLeafSize(0.5, 0.5, 0.5);

  // gtsam params
  ISAM2Params parameters;
  parameters.relinearizeThreshold = 0.01;
  parameters.relinearizeSkip = 1;
  isam = new ISAM2(parameters);

  // priorNoise
  gtsam::Vector priorNoiseVector6(6);
  priorNoiseVector6 << 1e-12, 1e-12, 1e-12, 1e-12, 1e-12, 1e-12;
  priorNoise = noiseModel::Diagonal::Variances(priorNoiseVector6);

  // odom factor noise
  gtsam::Vector odomNoiseVector6(6);
  // odomNoiseVector6 << 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4;
  odomNoiseVector6 << 1e-6, 1e-6, 1e-6, 1e-4, 1e-4, 1e-4;
  odomNoise = noiseModel::Diagonal::Variances(odomNoiseVector6);

  // loop factor noise
  double loopNoiseScore = 0.3; // constant is ok...
  gtsam::Vector robustNoiseVector6(6);
  robustNoiseVector6 << loopNoiseScore, loopNoiseScore, loopNoiseScore, loopNoiseScore, loopNoiseScore, loopNoiseScore;
  robustLoopNoise = gtsam::noiseModel::Robust::Create(
      gtsam::noiseModel::mEstimator::Cauchy::Create(1), // optional: replacing Cauchy by DCS or GemanMcClure is okay but Cauchy is empirically good.
      gtsam::noiseModel::Diagonal::Variances(robustNoiseVector6));

  // gps factor noise
  double bigNoiseTolerentToXY = 1000000000.0; // 1e9
  double gpsAltitudeNoiseScore = 250.0; // if height is misaligned after loop clsosing, use this value bigger
  gtsam::Vector robustNoiseVector3(3); // gps factor has 3 elements (xyz)
  robustNoiseVector3
      << bigNoiseTolerentToXY, bigNoiseTolerentToXY, gpsAltitudeNoiseScore; // means only caring altitude here. (because LOAM-like-methods tends to be asymptotically flyging)
  robustGPSNoise = gtsam::noiseModel::Robust::Create(
      gtsam::noiseModel::mEstimator::Cauchy::Create(1), // optional: replacing Cauchy by DCS or GemanMcClure is okay but Cauchy is empirically good.
      gtsam::noiseModel::Diagonal::Variances(robustNoiseVector3));
}

void PGO::OdomCB(const nav_msgs::Odometry::ConstPtr &msg) {
  mutex_.lock();
  odom_queue_.push(msg);
  mutex_.unlock();
}

void PGO::PcCB(const sensor_msgs::PointCloud2ConstPtr &_laserCloudFullRes) {
  mutex_.lock();
  cloud_queue_.push(_laserCloudFullRes);
  mutex_.unlock();
}

void PGO::GpsCB(const sensor_msgs::NavSatFix::ConstPtr &_gps) {
  if (useGPS) {
    mutex_.lock();
    gps_queue_.push(_gps);
    mutex_.unlock();
  }
}

void PGO::ISAM2Update() {
  // called when a variable added
  isam->update(gtSAMgraph, initialEstimate);
  isam->update();

  gtSAMgraph.resize(0);
  initialEstimate.clear();

  isamCurrentEstimate = isam->calculateEstimate();

  mKF.lock();
  for (int node_idx = 0; node_idx < int(isamCurrentEstimate.size()); node_idx++) {
    pcl::PointXYZI &p2 = keyposes_cloud_->points[node_idx];
    pcl::PointXYZI &p3 = keyframePoints->points[node_idx];
    Pose6D &p = keyframePosesUpdated[node_idx];
    p3.x = p2.x = p.x = isamCurrentEstimate.at<gtsam::Pose3>(node_idx).translation().x();
    p3.y = p2.y = p.y = isamCurrentEstimate.at<gtsam::Pose3>(node_idx).translation().y();
    p3.z = p2.z = p.z = isamCurrentEstimate.at<gtsam::Pose3>(node_idx).translation().z();
    p.roll = isamCurrentEstimate.at<gtsam::Pose3>(node_idx).rotation().roll();
    p.pitch = isamCurrentEstimate.at<gtsam::Pose3>(node_idx).rotation().pitch();
    p.yaw = isamCurrentEstimate.at<gtsam::Pose3>(node_idx).rotation().yaw();
  }
  mKF.unlock();

  mutex_pose_.lock();
  const gtsam::Pose3 &lastOptimizedPose = isamCurrentEstimate.at<gtsam::Pose3>(int(isamCurrentEstimate.size()) - 1);
  recentOptimizedX = lastOptimizedPose.translation().x();
  recentOptimizedY = lastOptimizedPose.translation().y();
  mutex_pose_.unlock();
}

void PGO::LoopFindNearKeyframesCloud(pcl::PointCloud<PointT>::Ptr &nearKeyframes,
                                     const int &key,
                                     const int &submap_size,
                                     const int &root_idx) {
  // extract and stacking near keyframes (in global coord)
  nearKeyframes->clear();
  for (int i = -submap_size; i <= submap_size; ++i) {
    int keyNear = root_idx + i;
    if (keyNear < 0 || keyNear >= keyframeLaserClouds.size())
      continue;

    mKF.lock();
    *nearKeyframes += *TransformCloud2Map(keyframeLaserClouds[keyNear], keyframePosesUpdated[keyNear]);
    mKF.unlock();
  }

  if (nearKeyframes->empty())
    return;

  // downsample near keyframes
  pcl::PointCloud<PointT>::Ptr cloud_temp(new pcl::PointCloud<PointT>());
  downSizeFilterICP.setInputCloud(nearKeyframes);
  downSizeFilterICP.filter(*cloud_temp);
  *nearKeyframes = *cloud_temp;
}

void PGO::Run() {
  if (!cloud_queue_.empty() && !odom_queue_.empty()) {
    mutex_.lock();
    double time2 = odom_queue_.front()->header.stamp.toSec();
    double time3 = cloud_queue_.front()->header.stamp.toSec();
    if (!odom_queue_.empty() && (time2 < time3 - 0.5 * 0.1)) {
      ROS_WARN("time stamp unaligned error and odometry discarded, pls check your data -->  optimization");
      odom_queue_.pop();
      mutex_.unlock();
      return;
    }
    if (!cloud_queue_.empty() && (time3 < time2 - 0.5 * 0.1)) {
      ROS_WARN("time stamp unaligned error and pointCloud discarded, pls check your data -->  optimization");
      cloud_queue_.pop();
      mutex_.unlock();
      return;
    }
    curr_odom_time_ = (cloud_queue_.front())->header.stamp.toSec();

    curr_frame_->clear();
    pcl::PointCloud<PointT>::Ptr thisKeyFrame(new pcl::PointCloud<PointT>());
    pcl::fromROSMsg(*cloud_queue_.front(), *thisKeyFrame);
    Pose6D pose_curr = Odom2Pose6D(odom_queue_.front());

    // find nearest gps
    double eps = 0.1; // find a gps topioc arrived within eps second
    if (!gps_queue_.empty()) {
      auto thisGPS = gps_queue_.front();
      auto thisGPSTime = thisGPS->header.stamp.toSec();
      if (abs(thisGPSTime - curr_odom_time_) < eps) {
        ROS_WARN("use gps");
        curr_gps_ = thisGPS;
        hasGPSforThisKF = true;
        return;
      } else {
        hasGPSforThisKF = false;
      }
      gps_queue_.pop();
    }
    cloud_queue_.pop();
    odom_queue_.pop();
    mutex_.unlock();

    odom_pose_prev = odom_pose_curr;
    odom_pose_curr = pose_curr;

    double delta_translation = std::sqrt(std::pow((odom_pose_prev.x - odom_pose_curr.x), 2)
                                             + std::pow((odom_pose_prev.y - odom_pose_curr.y), 2)
                                             + std::pow((odom_pose_prev.z - odom_pose_curr.z), 2));
    movementAccumulation += delta_translation;

    // 太近的舍弃
    if (movementAccumulation > keyframeMeterGap) {
      isNowKeyFrame = true;
      movementAccumulation = 0.0; // reset
    } else {
      isNowKeyFrame = false;
    }

    if (!isNowKeyFrame)
      return;

    if (!gpsOffsetInitialized) {
      if (hasGPSforThisKF) { // if the very first frame
        gpsAltitudeInitOffset = curr_gps_->altitude;
        gpsOffsetInitialized = true;
      }
    }

    // Save data and Add consecutive node
    mKF.lock();
    keyframeLaserClouds.push_back(thisKeyFrame);
    keyframePoses.push_back(pose_curr);
    originPoses.push_back(pose_curr);
    keyframePosesUpdated.push_back(pose_curr); // init
    keyframeTimes.push_back(curr_odom_time_);

    pcl::PointXYZI point;
    point.x = pose_curr.x;
    point.y = pose_curr.y;
    point.z = pose_curr.z;
    keyframePoints->points.push_back(point);
    point.z = 0;
    keyposes_cloud_->points.push_back(point);

    // use sc detector
    if (loop_method == 1) {
      scManager.makeAndSaveScancontextAndKeys(*thisKeyFrame);
    } else if (loop_method == 2) {
      pcl::PointCloud<pcl::PointXYZI>::Ptr pc_filtered(new pcl::PointCloud<pcl::PointXYZI>());
      iscGeneration.ground_filter(thisKeyFrame, pc_filtered); // 对z高度进行截取，粗略的去除地面
      ISCDescriptor desc = iscGeneration.calculate_isc(pc_filtered); // 计算非地面点云的sc特征
      Eigen::Vector3d current_t(pose_curr.x, pose_curr.y, pose_curr.z); // 当前odom位置

      //dont change push_back sequence
      if (iscGeneration.travel_distance_arr.size() == 0) {
        iscGeneration.travel_distance_arr.push_back(0);
      } else {
        // 这里是计算里程距离？
        double dis_temp = iscGeneration.travel_distance_arr.back()
            + std::sqrt((iscGeneration.pos_arr.back() - current_t).array().square().sum());
        iscGeneration.travel_distance_arr.push_back(dis_temp);
      }
      iscGeneration.pos_arr.push_back(current_t); // odom装到这里
      iscGeneration.isc_arr.push_back(desc); // isc特征全部装到这里面

      // 发布isc图像
      cv_bridge::CvImage out_msg;
      out_msg.header.frame_id = "camera_init";
      out_msg.header.stamp = ros::Time().fromSec(curr_odom_time_);
      out_msg.encoding = sensor_msgs::image_encodings::RGB8;
      out_msg.image = iscGeneration.getLastISCRGB();
      isc_pub.publish(out_msg.toImageMsg());
    }

    laserCloudMapPGORedraw = true;
    mKF.unlock();

    if (!gtSAMgraphMade) {
      /* prior node */
      const int init_node_idx = 0;
      gtsam::Pose3 poseOrigin = Pose6D2Pose3(keyframePoses.at(init_node_idx));
      mutex_pg_.lock();
      {
        // prior factor
        gtSAMgraph.add(gtsam::PriorFactor<gtsam::Pose3>(init_node_idx, poseOrigin, priorNoise));
        initialEstimate.insert(init_node_idx, poseOrigin);
        ISAM2Update();
      }
      mutex_pg_.unlock();

      gtSAMgraphMade = true;
      cout << "posegraph prior node " << init_node_idx << " added" << endl;
    } else {
      /* consecutive node (and odom factor) after the prior added */
      const int prev_node_idx = keyframePoses.size() - 2;
      const int curr_node_idx = keyframePoses.size() - 1;
      // becuase cpp starts with 0 (actually this index could be any number, but for simple implementation, we follow sequential indexing)
      gtsam::Pose3 poseFrom = Pose6D2Pose3(keyframePoses.at(prev_node_idx));
      gtsam::Pose3 poseTo = Pose6D2Pose3(keyframePoses.at(curr_node_idx));

      mutex_pg_.lock();
      {
        // odom factor
        gtSAMgraph.add(gtsam::BetweenFactor<gtsam::Pose3>(prev_node_idx,
                                                          curr_node_idx,
                                                          poseFrom.between(poseTo),
                                                          odomNoise));

        // gps factor
        if (hasGPSforThisKF) {
          double curr_altitude_offseted = curr_gps_->altitude - gpsAltitudeInitOffset;
          mutex_pose_.lock();
          // in this example, only adjusting altitude (for x and y, very big noises are set)
          gtsam::Point3 gpsConstraint(recentOptimizedX, recentOptimizedY, curr_altitude_offseted);
          mutex_pose_.unlock();
          gtSAMgraph.add(gtsam::GPSFactor(curr_node_idx, gpsConstraint, robustGPSNoise));
          cout << "GPS factor added at node " << curr_node_idx << endl;
        }
        initialEstimate.insert(curr_node_idx, poseTo);
        ISAM2Update();
      }
      mutex_pg_.unlock();

      // publish odom
      nav_msgs::Odometry odomAftPGOthis;
      odomAftPGOthis.header.frame_id = "camera_init";
      odomAftPGOthis.child_frame_id = "aft_mapped";
      odomAftPGOthis.header.stamp = ros::Time().fromSec(curr_odom_time_);
      odomAftPGOthis.pose.pose.position.x = keyframePosesUpdated.front().x;
      odomAftPGOthis.pose.pose.position.y = keyframePosesUpdated.front().y;
      odomAftPGOthis.pose.pose.position.z = keyframePosesUpdated.front().z;
      odomAftPGOthis.pose.pose.orientation =
          tf::createQuaternionMsgFromRollPitchYaw(keyframePosesUpdated.front().roll,
                                                  keyframePosesUpdated.front().pitch,
                                                  keyframePosesUpdated.front().yaw);
      final_odom_pub.publish(odomAftPGOthis); // last pose


      // tf
      static tf::TransformBroadcaster br;
      tf::Transform transform;
      tf::Quaternion q;
      transform.setOrigin(tf::Vector3(odomAftPGOthis.pose.pose.position.x,
                                      odomAftPGOthis.pose.pose.position.y,
                                      odomAftPGOthis.pose.pose.position.z));
      q.setW(odomAftPGOthis.pose.pose.orientation.w);
      q.setX(odomAftPGOthis.pose.pose.orientation.x);
      q.setY(odomAftPGOthis.pose.pose.orientation.y);
      q.setZ(odomAftPGOthis.pose.pose.orientation.z);
      transform.setRotation(q);
      br.sendTransform(tf::StampedTransform(transform, odomAftPGOthis.header.stamp, "camera_init", "aft_mapped"));

      // 100真输出一次node数量
      if (curr_node_idx % 100 == 0)
        cout << "posegraph odom node " << curr_node_idx << " added." << endl;
    }
  }
}

void PGO::PerformSCLoopClosure() {
  if (keyframePoses.size() < scManager.NUM_EXCLUDE_RECENT) // do not try too early
    return;

  if (loop_method == 1) {
    auto detectResult = scManager.detectLoopClosureID(); // first: nn index, second: yaw diff
    int SCclosestHistoryFrameID = detectResult.first;
    if (SCclosestHistoryFrameID != -1) {
      const int prev_node_idx = SCclosestHistoryFrameID;
      const int curr_node_idx = keyframePoses.size() - 1; // because cpp starts 0 and ends n-1
      ROS_WARN("Loop detected! - between %d and %d", prev_node_idx, curr_node_idx);

      double
          distance = sqrt(std::pow((keyframePosesUpdated[prev_node_idx].x - keyframePosesUpdated[curr_node_idx].x), 2) +
          std::pow((keyframePosesUpdated[prev_node_idx].y - keyframePosesUpdated[curr_node_idx].y), 2));
      std::cout << "distance: " << distance << std::endl;

      if (distance > 20.0) {
        ROS_ERROR("BAD SC RESULT..");
      } else {
        mutex_.lock();
        loop_queue_.push(std::pair<int, int>(prev_node_idx, curr_node_idx));
        // addding actual 6D constraints in the other thread, icp refine.
        mutex_.unlock();
      }
    }
  } else if (loop_method == 0) {
    // 寻找最近的pose
    int curr_node_idx = keyframePosesUpdated.size() - 1;

    pcl::PointXYZI currentRobotPosPoint;
    currentRobotPosPoint.x = keyframePosesUpdated[curr_node_idx].x;
    currentRobotPosPoint.y = keyframePosesUpdated[curr_node_idx].y;
    currentRobotPosPoint.z = 0;

    std::vector<int> pointSearchIndLoop;
    std::vector<float> pointSearchSqDisLoop;
    kdtreeHistoryKeyPoses->setInputCloud(keyframePoints);
    kdtreeHistoryKeyPoses->radiusSearch(currentRobotPosPoint,
                                        20.0,
                                        pointSearchIndLoop,
                                        pointSearchSqDisLoop,
                                        0);
    //std::cout << "pose number: " << keyposes_cloud_->size() << ", " << pointSearchIndLoop.size() << std::endl;

    int closestHistoryFrameID = -1;
    for (int i = 0; i < pointSearchIndLoop.size(); ++i) {
      int id = pointSearchIndLoop[i];
      if (std::abs(keyframeTimes[id] - curr_odom_time_) > 30.0) {
        closestHistoryFrameID = id;
        break;
      }
    }
    if (closestHistoryFrameID != -1) {
      int prev_node_idx = closestHistoryFrameID;
      double
          distance = sqrt(std::pow((keyframePosesUpdated[prev_node_idx].x - keyframePosesUpdated[curr_node_idx].x), 2) +
          std::pow((keyframePosesUpdated[prev_node_idx].y - keyframePosesUpdated[curr_node_idx].y), 2));
      std::cout << "distance: " << distance << std::endl;

      if (distance < 30.0) {
        mutex_.lock();
        loop_queue_.push(std::pair<int, int>(prev_node_idx, curr_node_idx));
        mutex_.unlock();
      } else {
        ROS_ERROR("BAD detection RESULT..");
      }
    }
  } else if (loop_method == 2) {
    // 使用ISC
    int curr_node_idx = keyframePosesUpdated.size() - 1;

    //search for the near neibourgh pos
    int prev_node_idx = 0;
    double best_score = 0.0;
    for (int i = 0; i < keyframePosesUpdated.size(); i++) { // 遍历之前点云帧的isc特征
      double delta_travel_distance =
          iscGeneration.travel_distance_arr.back() - iscGeneration.travel_distance_arr[i]; // 先检测是否存在距离最近的帧
      double pos_distance =
          std::sqrt((iscGeneration.pos_arr[i] - iscGeneration.pos_arr.back()).array().square().sum());  // 两帧空间距离
      // 我们认为里程距离越大，空间距离越小，越可能是回环
      if (delta_travel_distance > SKIP_NEIBOUR_DISTANCE
          && pos_distance < delta_travel_distance * INFLATION_COVARIANCE) {
        double geo_score = 0;
        double inten_score = 0;
        if (iscGeneration.is_loop_pair(iscGeneration.isc_arr[curr_node_idx],
                                       iscGeneration.isc_arr[i],
                                       geo_score,
                                       inten_score)) { // 计算isc的几何以及强度得分
          if (geo_score + inten_score > best_score) {  // 如果得分满足阈值，则认为是回环位置
            best_score = geo_score + inten_score;
            prev_node_idx = i;
          }
        }

      }
    }
    if (prev_node_idx != 0) {
      //matched_frame_id.push_back(best_matched_id);
      ROS_WARN("received loop closure candidate: current: %d, history %d, total_score %f",
               curr_node_idx,
               prev_node_idx,
               best_score);

      double
          distance = sqrt(std::pow((keyframePosesUpdated[prev_node_idx].x - keyframePosesUpdated[curr_node_idx].x), 2) +
          std::pow((keyframePosesUpdated[prev_node_idx].y - keyframePosesUpdated[curr_node_idx].y), 2));
      std::cout << "distance: " << distance << std::endl;
      if (distance < 20) {
        mutex_.lock();
        loop_queue_.push(std::pair<int, int>(prev_node_idx, curr_node_idx));
        mutex_.unlock();
      } else {
        ROS_ERROR("bad isc result");
      }
    }
  }
}

void PGO::LoopClosure() {
  ros::Rate rate(2.0);
  while (ros::ok()) {
    rate.sleep();
    PerformSCLoopClosure();

    // 位姿图可视化
    if (markers_pub.getNumSubscribers() && keyframePosesUpdated.size() > 2) {
      visualization_msgs::MarkerArray markers = CreateMarker(ros::Time::now());
      markers_pub.publish(markers);
    }
  }
}

void PGO::ICPRefine() {
  ros::Rate rate(5.0);
  while (ros::ok()) {
    if (!loop_queue_.empty()) {
      if (loop_queue_.size() > 30) {
        ROS_WARN("Too many loop clousre candidates to be ICPed is waiting ...");
      }

      mutex_.lock();
      std::pair<int, int> loop_idx_pair = loop_queue_.front();
      loop_queue_.pop();
      mutex_.unlock();

      // 开始icp
      const int _loop_kf_idx = loop_idx_pair.first;
      const int _curr_kf_idx = loop_idx_pair.second;

      // enough. ex. [-25, 25] covers submap length of 50x1 = 50m if every kf gap is 1m
      int historyKeyframeSearchNum = 25;
      pcl::PointCloud<PointT>::Ptr cureKeyframeCloud(new pcl::PointCloud<PointT>());
      pcl::PointCloud<PointT>::Ptr targetKeyframeCloud(new pcl::PointCloud<PointT>());
      LoopFindNearKeyframesCloud(cureKeyframeCloud, _curr_kf_idx, 0, _loop_kf_idx); // use same root of loop kf idx
      LoopFindNearKeyframesCloud(targetKeyframeCloud, _loop_kf_idx, historyKeyframeSearchNum, _loop_kf_idx);

      pcl::IterativeClosestPoint<PointT, PointT> icp;
      icp.setMaxCorrespondenceDistance(150); // giseop , use a value can cover 2*historyKeyframeSearchNum range in meter
      icp.setMaximumIterations(100);
      icp.setTransformationEpsilon(1e-6);
      icp.setEuclideanFitnessEpsilon(1e-6);
      icp.setRANSACIterations(0);

      // Align pointclouds
      icp.setInputSource(cureKeyframeCloud);
      icp.setInputTarget(targetKeyframeCloud);
      pcl::PointCloud<PointT>::Ptr unused_result(new pcl::PointCloud<PointT>());
      icp.align(*unused_result);

      double final_score = icp.getFitnessScore();
      float loopFitnessScoreThreshold = 0.3; // user parameter but fixed low value is safe.
      if (icp.hasConverged() == false || final_score > loopFitnessScoreThreshold) {
        ROS_INFO("LOOP REJECT %f", final_score);
        return;
      } else {
        ROS_WARN("LOOP DETECT  SUCCESS %f,", final_score);
      }

      // 闭环成功的存到这里
      loop_pairs_.push_back(std::pair<int, int>(_loop_kf_idx, _curr_kf_idx));

      gtsam::Vector robustNoiseVector6(6);
      robustNoiseVector6 << final_score, final_score, final_score, final_score, final_score, final_score;
      robustLoopNoise = gtsam::noiseModel::Robust::Create(
          gtsam::noiseModel::mEstimator::Cauchy::Create(1), // optional: replacing Cauchy by DCS or GemanMcClure is okay but Cauchy is empirically good.
          gtsam::noiseModel::Diagonal::Variances(robustNoiseVector6));

      // Get pose transformation
      float x, y, z, roll, pitch, yaw;
      Eigen::Affine3f correctionLidarFrame;
      correctionLidarFrame = icp.getFinalTransformation();
      pcl::getTranslationAndEulerAngles(correctionLidarFrame, x, y, z, roll, pitch, yaw);
      gtsam::Pose3 poseFrom = Pose3(Rot3::RzRyRx(roll, pitch, yaw), Point3(x, y, z));
      gtsam::Pose3 poseTo = Pose3(Rot3::RzRyRx(0.0, 0.0, 0.0), Point3(0.0, 0.0, 0.0));

      mutex_pg_.lock();
      gtSAMgraph.add(gtsam::BetweenFactor<gtsam::Pose3>(_loop_kf_idx,
                                                        _curr_kf_idx,
                                                        poseFrom.between(poseTo),
                                                        robustLoopNoise));
      ISAM2Update();
      mutex_pg_.unlock();
    }
  }
  rate.sleep();
}

void PGO::MapVisualization() {
  ros::Rate rate(0.1);
  while (ros::ok()) {
    rate.sleep();
    if (keyframeLaserClouds.size() > 1 && keyframePosesUpdated.size() > 1)
      PublishPoseAndFrame();
  }
  // 关闭终端是保存地图
  ROS_WARN("SAVE MAP AND G2O..");
  ISAM2Update(); // 最后一次更新全局地图
  SaveMap();
}

Pose6D PGO::Odom2Pose6D(nav_msgs::Odometry::ConstPtr _odom) {
  auto tx = _odom->pose.pose.position.x;
  auto ty = _odom->pose.pose.position.y;
  auto tz = _odom->pose.pose.position.z;

  double roll, pitch, yaw;
  geometry_msgs::Quaternion quat = _odom->pose.pose.orientation;
  tf::Matrix3x3(tf::Quaternion(quat.x, quat.y, quat.z, quat.w)).getRPY(roll, pitch, yaw);

  return Pose6D{tx, ty, tz, roll, pitch, yaw};
}

pcl::PointCloud<PointT>::Ptr PGO::TransformCloud2Map(const pcl::PointCloud<PointT>::Ptr &cloudIn, const Pose6D &tf) {
  pcl::PointCloud<PointT>::Ptr cloudOut(new pcl::PointCloud<PointT>());

  int cloudSize = cloudIn->size();
  cloudOut->resize(cloudSize);

  Eigen::Affine3f transCur = pcl::getTransformation(tf.x, tf.y, tf.z, tf.roll, tf.pitch, tf.yaw);

  int numberOfCores = 16;
#pragma omp parallel for num_threads(numberOfCores)
  for (int i = 0; i < cloudSize; ++i) {
    const auto &pointFrom = cloudIn->points[i];
    cloudOut->points[i].x =
        transCur(0, 0) * pointFrom.x + transCur(0, 1) * pointFrom.y + transCur(0, 2) * pointFrom.z + transCur(0, 3);
    cloudOut->points[i].y =
        transCur(1, 0) * pointFrom.x + transCur(1, 1) * pointFrom.y + transCur(1, 2) * pointFrom.z + transCur(1, 3);
    cloudOut->points[i].z =
        transCur(2, 0) * pointFrom.x + transCur(2, 1) * pointFrom.y + transCur(2, 2) * pointFrom.z + transCur(2, 3);
    cloudOut->points[i].intensity = pointFrom.intensity;
  }

  return cloudOut;
}

pcl::PointCloud<PointT>::Ptr PGO::TransformCloud2Map(pcl::PointCloud<PointT>::Ptr cloudIn,
                                                     gtsam::Pose3 transformIn) {
  pcl::PointCloud<PointT>::Ptr cloudOut(new pcl::PointCloud<PointT>());

  PointT *pointFrom;

  int cloudSize = cloudIn->size();
  cloudOut->resize(cloudSize);

  Eigen::Affine3f transCur = pcl::getTransformation(
      transformIn.translation().x(), transformIn.translation().y(), transformIn.translation().z(),
      transformIn.rotation().roll(), transformIn.rotation().pitch(), transformIn.rotation().yaw());

  int numberOfCores = 8; // TODO move to yaml
#pragma omp parallel for num_threads(numberOfCores)
  for (int i = 0; i < cloudSize; ++i) {
    pointFrom = &cloudIn->points[i];
    cloudOut->points[i].x =
        transCur(0, 0) * pointFrom->x + transCur(0, 1) * pointFrom->y + transCur(0, 2) * pointFrom->z + transCur(0, 3);
    cloudOut->points[i].y =
        transCur(1, 0) * pointFrom->x + transCur(1, 1) * pointFrom->y + transCur(1, 2) * pointFrom->z + transCur(1, 3);
    cloudOut->points[i].z =
        transCur(2, 0) * pointFrom->x + transCur(2, 1) * pointFrom->y + transCur(2, 2) * pointFrom->z + transCur(2, 3);
    cloudOut->points[i].intensity = pointFrom->intensity;
  }
  return cloudOut;
} // transformPointCloud

void PGO::SaveMap() {
  std::stringstream ss;
  ss << int(ros::Time::now().toSec());
  std::string stamp = ss.str();

  std::string file_path = save_dir_ + stamp + "/";
  if (0 != access(file_path.c_str(), 2)) {
    mkdir(file_path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    ROS_INFO("mkdir filepath %s", file_path.c_str());
  }

  int num_points = 0, num_points1 = 0;
  pcl::PointCloud<PointT>::Ptr map(new pcl::PointCloud<PointT>());
  for (int i = 0; i < keyframeLaserClouds.size(); ++i) {
    *map += *TransformCloud2Map(keyframeLaserClouds[i], keyframePosesUpdated[i]);
  }
  map->width = map->points.size();
  map->height = 1;
  map->is_dense = false;

  //  map_no_ground->width = map_no_ground->points.size();
  //  map_no_ground->height = 1;
  //  map_no_ground->is_dense = false;
  pcl::PointCloud<PointT>::Ptr poses(new pcl::PointCloud<PointT>());
  pcl::copyPointCloud(*keyframePoints, *poses);
  poses->width = poses->points.size();
  poses->height = 1;
  poses->is_dense = false;
  downSizeFilterMapPGO.setInputCloud(map);
  downSizeFilterMapPGO.filter(*map);

  pcl::io::savePCDFile(file_path + "trajectory.pcd", *poses);
  pcl::io::savePCDFile(file_path + "finalMap.pcd", *map);

  // 保存odom的csv
  ROS_WARN("save odom csv files and g2o");
  std::ofstream outFile, outFile2;
  outFile.open(file_path + "odom_final.csv", std::ios::out);
  outFile2.open(file_path + "odom.csv", std::ios::out);
  outFile << "stamp,x,y,z,roll,pitch,yaw" << std::endl;
  outFile2 << "stamp,x,y,z,roll,pitch,yaw" << std::endl;
  for (int j = 0; j < keyframePosesUpdated.size(); ++j) {
    outFile << keyframeTimes[j] << ","
            << keyframePosesUpdated[j].x << "," << keyframePosesUpdated[j].y << "," << keyframePosesUpdated[j].z << ","
            << keyframePosesUpdated[j].roll << "," << keyframePosesUpdated[j].pitch << ","
            << keyframePosesUpdated[j].yaw
            <<
            endl;

    outFile2 << keyframeTimes[j] << ","
             << originPoses[j].x << "," << originPoses[j].y << "," << originPoses[j].z << ","
             << originPoses[j].roll << "," << originPoses[j].pitch << "," << originPoses[j].yaw
             <<
             endl;
  }
  outFile.close();
  outFile2.close();

  // 同事保存g2o文件
  gtsam::writeG2o(gtSAMgraph, isamCurrentEstimate, file_path + "pose_graph.g2o");
  ROS_WARN("Save map. pose size: %d, cloud size: %d", poses->points.size(), num_points);
}

void PGO::PublishPoseAndFrame() {
  // pub odom and path
//  keyframePoints->clear();
  // nav_msgs::Odometry odomAftPGO;
  if (keyframePoints->empty() || keyframePosesUpdated.size() < 1)
    return;

  // publish map every 2 frame
  int SKIP_FRAMES = 2;
  int counter = 0;
  laserCloudMapPGO->clear();

  mKF.lock();
  for (int node_idx = 0; node_idx < int(keyframePosesUpdated.size()) - 1;
       node_idx++) // -1 is just delayed visualization (because sometimes mutexed while adding(push_back) a new one)
  {
    const Pose6D &pose_est = keyframePosesUpdated.at(node_idx); // upodated poses

    /* nav_msgs::Odometry odomAftPGOthis;
     odomAftPGOthis.header.frame_id = "map";
     odomAftPGOthis.child_frame_id = "velo_link";
     odomAftPGOthis.header.stamp = ros::Time().fromSec(keyframeTimes.at(node_idx));
     odomAftPGOthis.pose.pose.position.x = pose_est.x;
     odomAftPGOthis.pose.pose.position.y = pose_est.y;
     odomAftPGOthis.pose.pose.position.z = pose_est.z;
     odomAftPGOthis.pose.pose.orientation =
         tf::createQuaternionMsgFromRollPitchYaw(pose_est.roll, pose_est.pitch, pose_est.yaw);
     odomAftPGO = odomAftPGOthis;*/

//    geometry_msgs::PoseStamped poseStampAftPGO;
//    poseStampAftPGO.header = odomAftPGOthis.header;
//    poseStampAftPGO.pose = odomAftPGOthis.pose.pose;

//    pcl::PointXYZI pt;
//    pt.x = pose_est.x;
//    pt.y = pose_est.y;
//    pt.z = pose_est.z;
//    keyframePoints->points.push_back(pt);

    if (counter % SKIP_FRAMES == 0) {
      *laserCloudMapPGO += *TransformCloud2Map(keyframeLaserClouds[node_idx], keyframePosesUpdated[node_idx]);
    }
    counter++;
  }
  mKF.unlock();

//  final_odom_pub.publish(odomAftPGO); // last pose

/*  // tf
  static tf::TransformBroadcaster br;
  tf::Transform transform;
  tf::Quaternion q;
  transform.setOrigin(tf::Vector3(odomAftPGO.pose.pose.position.x,
                                  odomAftPGO.pose.pose.position.y,
                                  odomAftPGO.pose.pose.position.z));
  q.setW(odomAftPGO.pose.pose.orientation.w);
  q.setX(odomAftPGO.pose.pose.orientation.x);
  q.setY(odomAftPGO.pose.pose.orientation.y);
  q.setZ(odomAftPGO.pose.pose.orientation.z);
  transform.setRotation(q);
  br.sendTransform(tf::StampedTransform(transform, odomAftPGO.header.stamp, "map", "velo_link"));*/

  // key poses
  sensor_msgs::PointCloud2 msg;
  pcl::toROSMsg(*keyframePoints, msg);
  msg.header.frame_id = "camera_init";
  msg.header.stamp = ros::Time::now();
  pose_pub.publish(msg);

  // publish map
  downSizePublishCloud.setInputCloud(laserCloudMapPGO);
  downSizePublishCloud.filter(*laserCloudMapPGO);
  sensor_msgs::PointCloud2 laserCloudMapPGOMsg;
  pcl::toROSMsg(*laserCloudMapPGO, laserCloudMapPGOMsg);
  laserCloudMapPGOMsg.header.frame_id = "camera_init";
  map_pub.publish(laserCloudMapPGOMsg);

  // isc
/*  if (loop_method == 2) {
    // 发布isc图像
    cv_bridge::CvImage out_msg;
    out_msg.header.frame_id = "camera_init";
    out_msg.header.stamp = ros::Time().fromSec(curr_odom_time_);
    out_msg.encoding = sensor_msgs::image_encodings::RGB8;
    out_msg.image = iscGeneration.getLastISCRGB();
    isc_pub.publish(out_msg.toImageMsg());
  }*/
}

visualization_msgs::MarkerArray PGO::CreateMarker(const ros::Time &stamp) {
  visualization_msgs::MarkerArray marker_array;
  visualization_msgs::Marker traj_marker, edge_marker, loop_marker;

  // node markers
  traj_marker.header.frame_id = "camera_init";
  traj_marker.header.stamp = stamp;
  traj_marker.ns = "nodes";
  traj_marker.id = 0;
  traj_marker.type = visualization_msgs::Marker::SPHERE_LIST;
  traj_marker.pose.orientation.w = 1.0;
  traj_marker.scale.x = traj_marker.scale.y = traj_marker.scale.z = 1.5;
  std_msgs::ColorRGBA color;
  color.r = 0.0;
  color.g = 0.0;
  color.b = 1.0;
  color.a = 1.0;
  traj_marker.color = color;

  // edge markers
  edge_marker.header.frame_id = "camera_init";
  edge_marker.header.stamp = stamp;
  edge_marker.ns = "edges";
  edge_marker.id = 1;
  edge_marker.type = visualization_msgs::Marker::LINE_STRIP;
  traj_marker.action = edge_marker.action = visualization_msgs::Marker::ADD;

  edge_marker.pose.orientation.w = 1.0;
  edge_marker.scale.x = 0.5;
  edge_marker.color.a = 0.5;
  edge_marker.color.r = 0.0;
  edge_marker.color.g = 1.0;
  edge_marker.color.b = 0.0;

  for (int j = 0; j < keyframePosesUpdated.size() - 1; ++j) {
    geometry_msgs::Point pt, pt2;
    pt.x = keyframePosesUpdated[j].x;
    pt.y = keyframePosesUpdated[j].y;
    pt.z = keyframePosesUpdated[j].z;

    pt2.x = keyframePosesUpdated[j + 1].x;
    pt2.y = keyframePosesUpdated[j + 1].y;
    pt2.z = keyframePosesUpdated[j + 1].z;

    traj_marker.points.push_back(pt);
    edge_marker.points.push_back(pt);
    edge_marker.points.push_back(pt2);
    marker_array.markers.push_back(edge_marker);
    marker_array.markers.push_back(traj_marker);
  }


  // loop markers
  if (loop_pairs_.size() > 0) {
    loop_marker.header.frame_id = "camera_init";
    loop_marker.header.stamp = stamp;
    loop_marker.ns = "loop";
    loop_marker.id = 2;
    loop_marker.type = visualization_msgs::Marker::LINE_STRIP;

    loop_marker.pose.orientation.w = 1.0;
    loop_marker.scale.x = loop_marker.scale.y = loop_marker.scale.z = 1.0;
    loop_marker.color.r = 1.0;
    loop_marker.color.g = 0.0;
    loop_marker.color.b = 0.0;
    loop_marker.color.a = 0.5;

    for (int j = 0; j < loop_pairs_.size(); ++j) {
      int loop_id = loop_pairs_[j].first;
      int curr_id = loop_pairs_[j].second;

      geometry_msgs::Point pt, pt2;
      pt.x = keyframePosesUpdated[loop_id].x;
      pt.y = keyframePosesUpdated[loop_id].y;
      pt.z = keyframePosesUpdated[loop_id].z;
      pt2.x = keyframePosesUpdated[curr_id].x;
      pt2.y = keyframePosesUpdated[curr_id].y;
      pt2.z = keyframePosesUpdated[curr_id].z;

      loop_marker.points.push_back(pt);
      loop_marker.points.push_back(pt2);
      marker_array.markers.push_back(loop_marker);
    }
  }

  return marker_array;
}
