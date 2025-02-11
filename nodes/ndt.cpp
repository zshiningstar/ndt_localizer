#include "ndt.h"

NdtLocalizer::NdtLocalizer(ros::NodeHandle &nh, ros::NodeHandle &private_nh):nh_(nh), private_nh_(private_nh), tf2_listener_(tf2_buffer_){

  key_value_stdmap_["state"] = "Initializing";
  init_params();

  // Publishers
  sensor_aligned_pose_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("points_aligned", 10);//发布对齐的点云
  ndt_pose_pub_ = nh_.advertise<geometry_msgs::PoseStamped>("ndt_pose", 10);//发布车辆位姿
  exe_time_pub_ = nh_.advertise<std_msgs::Float32>("exe_time_ms", 10);//发布计算时间
  transform_probability_pub_ = nh_.advertise<std_msgs::Float32>("transform_probability", 10);
  iteration_num_pub_ = nh_.advertise<std_msgs::Float32>("iteration_num", 10);//迭代次数
  diagnostics_pub_ = nh_.advertise<diagnostic_msgs::DiagnosticArray>("diagnostics", 10);

  // Subscribers
  initial_pose_sub_ = nh_.subscribe("/initialpose", 100, &NdtLocalizer::callback_init_pose, this);//初始姿态
  map_points_sub_ = nh_.subscribe("points_map", 1, &NdtLocalizer::callback_pointsmap, this);//pcd点云地图
  sensor_points_sub_ = nh_.subscribe("filtered_points", 1, &NdtLocalizer::callback_pointcloud, this);//降采样后点云

  diagnostic_thread_ = std::thread(&NdtLocalizer::timer_diagnostic, this);
  diagnostic_thread_.detach();
}

NdtLocalizer::~NdtLocalizer() {}

void NdtLocalizer::timer_diagnostic()
{
  ros::Rate rate(100);
  while (ros::ok()) {
    diagnostic_msgs::DiagnosticStatus diag_status_msg;
    diag_status_msg.name = "ndt_scan_matcher";
    diag_status_msg.hardware_id = "";

    for (const auto & key_value : key_value_stdmap_) {
      diagnostic_msgs::KeyValue key_value_msg;
      key_value_msg.key = key_value.first;
      key_value_msg.value = key_value.second;
      diag_status_msg.values.push_back(key_value_msg);
    }

    diag_status_msg.level = diagnostic_msgs::DiagnosticStatus::OK;
    diag_status_msg.message = "";
    if (key_value_stdmap_.count("state") && key_value_stdmap_["state"] == "Initializing") {
      diag_status_msg.level = diagnostic_msgs::DiagnosticStatus::WARN;
      diag_status_msg.message += "Initializing State. ";
    }
    if (
      key_value_stdmap_.count("skipping_publish_num") &&
      std::stoi(key_value_stdmap_["skipping_publish_num"]) > 1) {
      diag_status_msg.level = diagnostic_msgs::DiagnosticStatus::WARN;
      diag_status_msg.message += "skipping_publish_num > 1. ";
    }
    if (
      key_value_stdmap_.count("skipping_publish_num") &&
      std::stoi(key_value_stdmap_["skipping_publish_num"]) >= 5) {
      diag_status_msg.level = diagnostic_msgs::DiagnosticStatus::ERROR;
      diag_status_msg.message += "skipping_publish_num exceed limit. ";
    }

    diagnostic_msgs::DiagnosticArray diag_msg;
    diag_msg.header.stamp = ros::Time::now();
    diag_msg.status.push_back(diag_status_msg);

    diagnostics_pub_.publish(diag_msg);

    rate.sleep();
  }
}

//将初始位姿变换到map坐标系下，并用initial_pose_cov_msg_表示
void NdtLocalizer::callback_init_pose(
  const geometry_msgs::PoseWithCovarianceStamped::ConstPtr & initial_pose_msg_ptr)
{
  if (initial_pose_msg_ptr->header.frame_id == map_frame_) {//map
  
    initial_pose_cov_msg_ = *initial_pose_msg_ptr;
  } else {
    // get TF from pose_frame to map_frame
    //得到初始位姿到地图下的tf转换
    geometry_msgs::TransformStamped::Ptr TF_pose_to_map_ptr(new geometry_msgs::TransformStamped);
    get_transform(map_frame_, initial_pose_msg_ptr->header.frame_id, TF_pose_to_map_ptr);

    // transform pose_frame to map_frame
    geometry_msgs::PoseWithCovarianceStamped::Ptr mapTF_initial_pose_msg_ptr(
      new geometry_msgs::PoseWithCovarianceStamped);
    tf2::doTransform(*initial_pose_msg_ptr, *mapTF_initial_pose_msg_ptr, *TF_pose_to_map_ptr);
    // mapTF_initial_pose_msg_ptr->header.stamp = initial_pose_msg_ptr->header.stamp;
    initial_pose_cov_msg_ = *mapTF_initial_pose_msg_ptr;
  }
  // if click the initpose again, re init！
  init_pose = false;
}

//将pcd点云设置为ndt的目标点云,并设置ndt各个参数
//订阅map_loader中载入pcd点云后发布的话题消息
void NdtLocalizer::callback_pointsmap(
  const sensor_msgs::PointCloud2::ConstPtr & map_points_msg_ptr)
{
  const auto trans_epsilon = ndt_.getTransformationEpsilon();
  const auto step_size = ndt_.getStepSize();
  const auto resolution = ndt_.getResolution();
  const auto max_iterations = ndt_.getMaximumIterations();

  pcl::NormalDistributionsTransform<pcl::PointXYZ, pcl::PointXYZ> ndt_new;

  ndt_new.setTransformationEpsilon(trans_epsilon);
  ndt_new.setStepSize(step_size);
  ndt_new.setResolution(resolution);
  ndt_new.setMaximumIterations(max_iterations);

  pcl::PointCloud<pcl::PointXYZ>::Ptr map_points_ptr(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::fromROSMsg(*map_points_msg_ptr, *map_points_ptr);//转为ros消息
  ndt_new.setInputTarget(map_points_ptr);//设置目标点云
  // create Thread
  // detach
  pcl::PointCloud<pcl::PointXYZ>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZ>);
  ndt_new.align(*output_cloud, Eigen::Matrix4f::Identity());

  // swap
  ndt_map_mtx_.lock();
  ndt_ = ndt_new;
  ndt_map_mtx_.unlock();
}

//NDT配准定位,获取降采样点之后
void NdtLocalizer::callback_pointcloud(
  const sensor_msgs::PointCloud2::ConstPtr & sensor_points_sensorTF_msg_ptr)
{
  const auto exe_start_time = std::chrono::system_clock::now();
  // mutex Map
  std::lock_guard<std::mutex> lock(ndt_map_mtx_);

  const std::string sensor_frame = sensor_points_sensorTF_msg_ptr->header.frame_id;//接收到传感器点云时的坐标系
  const auto sensor_ros_time = sensor_points_sensorTF_msg_ptr->header.stamp;//接收到传感器点云时间戳

  boost::shared_ptr<pcl::PointCloud<pcl::PointXYZ>> sensor_points_sensorTF_ptr(
    new pcl::PointCloud<pcl::PointXYZ>);

  pcl::fromROSMsg(*sensor_points_sensorTF_msg_ptr, *sensor_points_sensorTF_ptr);
  // get TF base to sensor
  //将位激光雷达坐标系下的数据投射到base_link下
  geometry_msgs::TransformStamped::Ptr TF_base_to_sensor_ptr(new geometry_msgs::TransformStamped);
  //获取sensor到base的tf变换，并存到TF_base_to_sensor_ptr
  get_transform(base_frame_, sensor_frame, TF_base_to_sensor_ptr);
  
  const Eigen::Affine3d base_to_sensor_affine = tf2::transformToEigen(*TF_base_to_sensor_ptr);
  //获取从sensor到base的转换矩阵
  const Eigen::Matrix4f base_to_sensor_matrix = base_to_sensor_affine.matrix().cast<float>();

  boost::shared_ptr<pcl::PointCloud<pcl::PointXYZ>> sensor_points_baselinkTF_ptr(
    new pcl::PointCloud<pcl::PointXYZ>);
  //将sensor点云通过base_to_sensor_matrix转换到base坐标系，结果保存到*sensor_points_baselinkTF_ptr
  pcl::transformPointCloud(
    *sensor_points_sensorTF_ptr, *sensor_points_baselinkTF_ptr, base_to_sensor_matrix);
  
  // set input point cloud
  //将转换到base下的sensor点云设置为ndt的输入源
  ndt_.setInputSource(sensor_points_baselinkTF_ptr);

  if (ndt_.getInputTarget() == nullptr) {//为空,说明地图无载入成功
    ROS_WARN_STREAM_THROTTLE(1, "No MAP!");
    return;
  }
  // align
  Eigen::Matrix4f initial_pose_matrix;
  if (!init_pose){//初次配准
    Eigen::Affine3d initial_pose_affine;
    //将pose转为Eigen::Matrix4f
    //initial_pose_cov_msg_:初始位姿变换
    tf2::fromMsg(initial_pose_cov_msg_.pose.pose, initial_pose_affine);
    initial_pose_matrix = initial_pose_affine.matrix().cast<float>();
    // for the first time, we don't know the pre_trans, so just use the init_trans, 
    // which means, the delta trans for the second time is 0
    pre_trans = initial_pose_matrix;
    init_pose = true;
  }else
  {
    // use predicted pose as init guess (currently we only impl linear model)
    //将上一帧求得的位姿作为初始位姿,利用线性模型做当前帧位姿的估计
    initial_pose_matrix = pre_trans * delta_trans;
  }
  
  pcl::PointCloud<pcl::PointXYZ>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZ>);
  const auto align_start_time = std::chrono::system_clock::now();
  key_value_stdmap_["state"] = "Aligning";
  //使用ndt配准
  ndt_.align(*output_cloud, initial_pose_matrix);//配准
  key_value_stdmap_["state"] = "Sleeping";
  const auto align_end_time = std::chrono::system_clock::now();
  const double align_time = std::chrono::duration_cast<std::chrono::microseconds>(align_end_time - align_start_time).count() /1000.0;//配准用时

  const Eigen::Matrix4f result_pose_matrix = ndt_.getFinalTransformation();//得到最终变换
  Eigen::Affine3d result_pose_affine;
  result_pose_affine.matrix() = result_pose_matrix.cast<double>();
  const geometry_msgs::Pose result_pose_msg = tf2::toMsg(result_pose_affine);

  const auto exe_end_time = std::chrono::system_clock::now();
  const double exe_time = std::chrono::duration_cast<std::chrono::microseconds>(exe_end_time - exe_start_time).count() / 1000.0;

  const float transform_probability = ndt_.getTransformationProbability();
  const int iteration_num = ndt_.getFinalNumIteration();
  
  //收敛判别
  bool is_converged = true;
  static size_t skipping_publish_num = 0;
  if (
    iteration_num >= ndt_.getMaximumIterations() + 2 ||
    transform_probability < converged_param_transform_probability_) {
    is_converged = false;
    ++skipping_publish_num;
    std::cout << "Not Converged" << std::endl;
  } else {
    skipping_publish_num = 0;
  }
  // calculate the delta tf from pre_trans to current_trans
  delta_trans = pre_trans.inverse() * result_pose_matrix;

  Eigen::Vector3f delta_translation = delta_trans.block<3, 1>(0, 3);
  std::cout<<"delta x: "<<delta_translation(0) << " y: "<<delta_translation(1)<<
             " z: "<<delta_translation(2)<<std::endl;

  Eigen::Matrix3f delta_rotation_matrix = delta_trans.block<3, 3>(0, 0);
  Eigen::Vector3f delta_euler = delta_rotation_matrix.eulerAngles(2,1,0);
  std::cout<<"delta yaw: "<<delta_euler(0) << " pitch: "<<delta_euler(1)<<
             " roll: "<<delta_euler(2)<<std::endl;

  pre_trans = result_pose_matrix;
  
  // publish
  geometry_msgs::PoseStamped result_pose_stamped_msg;
  result_pose_stamped_msg.header.stamp = sensor_ros_time;
  result_pose_stamped_msg.header.frame_id = map_frame_;
  result_pose_stamped_msg.pose = result_pose_msg;

  if (is_converged) {
    ndt_pose_pub_.publish(result_pose_stamped_msg);
  }

  // publish tf(map frame to base frame)
  publish_tf(map_frame_, base_frame_, result_pose_stamped_msg);

  // publish aligned point cloud
  pcl::PointCloud<pcl::PointXYZ>::Ptr sensor_points_mapTF_ptr(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::transformPointCloud(
    *sensor_points_baselinkTF_ptr, *sensor_points_mapTF_ptr, result_pose_matrix);
  sensor_msgs::PointCloud2 sensor_points_mapTF_msg;
  pcl::toROSMsg(*sensor_points_mapTF_ptr, sensor_points_mapTF_msg);
  sensor_points_mapTF_msg.header.stamp = sensor_ros_time;
  sensor_points_mapTF_msg.header.frame_id = map_frame_;
  sensor_aligned_pose_pub_.publish(sensor_points_mapTF_msg);


  std_msgs::Float32 exe_time_msg;
  exe_time_msg.data = exe_time;
  exe_time_pub_.publish(exe_time_msg);

  std_msgs::Float32 transform_probability_msg;
  transform_probability_msg.data = transform_probability;
  transform_probability_pub_.publish(transform_probability_msg);

  std_msgs::Float32 iteration_num_msg;
  iteration_num_msg.data = iteration_num;
  iteration_num_pub_.publish(iteration_num_msg);

  key_value_stdmap_["seq"] = std::to_string(sensor_points_sensorTF_msg_ptr->header.seq);
  key_value_stdmap_["transform_probability"] = std::to_string(transform_probability);
  key_value_stdmap_["iteration_num"] = std::to_string(iteration_num);
  key_value_stdmap_["skipping_publish_num"] = std::to_string(skipping_publish_num);

  std::cout << "------------------------------------------------" << std::endl;
  std::cout << "align_time: " << align_time << "ms" << std::endl;
  std::cout << "exe_time: " << exe_time << "ms" << std::endl;
  std::cout << "trans_prob: " << transform_probability << std::endl;
  std::cout << "iter_num: " << iteration_num << std::endl;
  std::cout << "skipping_publish_num: " << skipping_publish_num << std::endl;
}

//
void NdtLocalizer::init_params(){

  private_nh_.getParam("base_frame", base_frame_);//base_link
  ROS_INFO("base_frame_id: %s", base_frame_.c_str());
  //最小搜索变化量,即前后两次迭代转换矩阵的最大容差,一旦两次迭代小于这个容差,则认为已经收敛到最优解,迭代停止
  double trans_epsilon = ndt_.getTransformationEpsilon();
  double step_size = ndt_.getStepSize();//搜索步长度
  double resolution = ndt_.getResolution();//目标点云的ND体素,单位m
  int max_iterations = ndt_.getMaximumIterations();//使用牛顿法优化的迭代次数

  private_nh_.getParam("trans_epsilon", trans_epsilon);
  private_nh_.getParam("step_size", step_size);
  private_nh_.getParam("resolution", resolution);
  private_nh_.getParam("max_iterations", max_iterations);

  map_frame_ = "map";
  //设置ndt一些参数
  ndt_.setTransformationEpsilon(trans_epsilon);
  ndt_.setStepSize(step_size);
  ndt_.setResolution(resolution);
  ndt_.setMaximumIterations(max_iterations);

  ROS_INFO(
    "trans_epsilon: %lf, step_size: %lf, resolution: %lf, max_iterations: %d", trans_epsilon,
    step_size, resolution, max_iterations);

  private_nh_.getParam(
    "converged_param_transform_probability", converged_param_transform_probability_);
}

//获取坐标变换关系
bool NdtLocalizer::get_transform(
  const std::string & target_frame, const std::string & source_frame,
  const geometry_msgs::TransformStamped::Ptr & transform_stamped_ptr, const ros::Time & time_stamp)
{
  if (target_frame == source_frame) {
    transform_stamped_ptr->header.stamp = time_stamp;
    transform_stamped_ptr->header.frame_id = target_frame;
    transform_stamped_ptr->child_frame_id = source_frame;
    transform_stamped_ptr->transform.translation.x = 0.0;
    transform_stamped_ptr->transform.translation.y = 0.0;
    transform_stamped_ptr->transform.translation.z = 0.0;
    transform_stamped_ptr->transform.rotation.x = 0.0;
    transform_stamped_ptr->transform.rotation.y = 0.0;
    transform_stamped_ptr->transform.rotation.z = 0.0;
    transform_stamped_ptr->transform.rotation.w = 1.0;
    return true;
  }

  try {
    *transform_stamped_ptr =
      tf2_buffer_.lookupTransform(target_frame, source_frame, time_stamp);
  } catch (tf2::TransformException & ex) {
    ROS_WARN("%s", ex.what());
    ROS_ERROR("Please publish TF %s to %s", target_frame.c_str(), source_frame.c_str());

    transform_stamped_ptr->header.stamp = time_stamp;
    transform_stamped_ptr->header.frame_id = target_frame;
    transform_stamped_ptr->child_frame_id = source_frame;
    transform_stamped_ptr->transform.translation.x = 0.0;
    transform_stamped_ptr->transform.translation.y = 0.0;
    transform_stamped_ptr->transform.translation.z = 0.0;
    transform_stamped_ptr->transform.rotation.x = 0.0;
    transform_stamped_ptr->transform.rotation.y = 0.0;
    transform_stamped_ptr->transform.rotation.z = 0.0;
    transform_stamped_ptr->transform.rotation.w = 1.0;
    return false;
  }
  return true;
}

bool NdtLocalizer::get_transform(
  const std::string & target_frame, const std::string & source_frame,
  const geometry_msgs::TransformStamped::Ptr & transform_stamped_ptr)
{
  if (target_frame == source_frame) {
    transform_stamped_ptr->header.stamp = ros::Time::now();
    transform_stamped_ptr->header.frame_id = target_frame;
    transform_stamped_ptr->child_frame_id = source_frame;
    transform_stamped_ptr->transform.translation.x = 0.0;
    transform_stamped_ptr->transform.translation.y = 0.0;
    transform_stamped_ptr->transform.translation.z = 0.0;
    transform_stamped_ptr->transform.rotation.x = 0.0;
    transform_stamped_ptr->transform.rotation.y = 0.0;
    transform_stamped_ptr->transform.rotation.z = 0.0;
    transform_stamped_ptr->transform.rotation.w = 1.0;
    return true;
  }

  try {
    *transform_stamped_ptr =
      tf2_buffer_.lookupTransform(target_frame, source_frame, ros::Time(0), ros::Duration(1.0));
  } catch (tf2::TransformException & ex) {
    ROS_WARN("%s", ex.what());
    ROS_ERROR("Please publish TF %s to %s", target_frame.c_str(), source_frame.c_str());

    transform_stamped_ptr->header.stamp = ros::Time::now();
    transform_stamped_ptr->header.frame_id = target_frame;
    transform_stamped_ptr->child_frame_id = source_frame;
    transform_stamped_ptr->transform.translation.x = 0.0;
    transform_stamped_ptr->transform.translation.y = 0.0;
    transform_stamped_ptr->transform.translation.z = 0.0;
    transform_stamped_ptr->transform.rotation.x = 0.0;
    transform_stamped_ptr->transform.rotation.y = 0.0;
    transform_stamped_ptr->transform.rotation.z = 0.0;
    transform_stamped_ptr->transform.rotation.w = 1.0;
    return false;
  }
  return true;
}

void NdtLocalizer::publish_tf(
  const std::string & frame_id, const std::string & child_frame_id,
  const geometry_msgs::PoseStamped & pose_msg)
{
  geometry_msgs::TransformStamped transform_stamped;
  transform_stamped.header.frame_id = frame_id;
  transform_stamped.child_frame_id = child_frame_id;
  transform_stamped.header.stamp = pose_msg.header.stamp;

  transform_stamped.transform.translation.x = pose_msg.pose.position.x;
  transform_stamped.transform.translation.y = pose_msg.pose.position.y;
  transform_stamped.transform.translation.z = pose_msg.pose.position.z;

  tf2::Quaternion tf_quaternion;
  tf2::fromMsg(pose_msg.pose.orientation, tf_quaternion);
  transform_stamped.transform.rotation.x = tf_quaternion.x();
  transform_stamped.transform.rotation.y = tf_quaternion.y();
  transform_stamped.transform.rotation.z = tf_quaternion.z();
  transform_stamped.transform.rotation.w = tf_quaternion.w();

  tf2_broadcaster_.sendTransform(transform_stamped);
}


int main(int argc, char **argv)
{
    ros::init(argc, argv, "ndt_localizer");
    ros::NodeHandle nh;
    ros::NodeHandle private_nh("~");

    NdtLocalizer ndt_localizer(nh, private_nh);

    ros::spin();

    return 0;
}
