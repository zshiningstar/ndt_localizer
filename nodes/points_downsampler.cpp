//载入地图之后,启动雷达进行NDT配准时,为了提高配准效率,采用降采样对输入点云进行降采样处理
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>

#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/filters/voxel_grid.h>

// #include "points_downsampler.h"

#define MAX_MEASUREMENT_RANGE 120.0

ros::Publisher filtered_points_pub;

// Leaf size of VoxelGrid filter.
static double voxel_leaf_size = 2.0;

static bool _output_log = false;
static std::ofstream ofs;
static std::string filename;

static std::string POINTS_TOPIC;

static pcl::PointCloud<pcl::PointXYZ> removePointsByRange(pcl::PointCloud<pcl::PointXYZ> scan, double min_range, double max_range)
{
  pcl::PointCloud<pcl::PointXYZ> narrowed_scan;
  narrowed_scan.header = scan.header;

  if( min_range>=max_range ) {
    ROS_ERROR_ONCE("min_range>=max_range @(%lf, %lf)", min_range, max_range );
    return scan;
  }

  double square_min_range = min_range * min_range;
  double square_max_range = max_range * max_range;

  for(pcl::PointCloud<pcl::PointXYZ>::const_iterator iter = scan.begin(); iter != scan.end(); ++iter)
  {
    const pcl::PointXYZ &p = *iter;
    double square_distance = p.x * p.x + p.y * p.y;

    if(square_min_range <= square_distance && square_distance <= square_max_range){
      narrowed_scan.points.push_back(p);
    }
  }

  return narrowed_scan;
}

//得到点云后,首先对点云进行截取,只保留MAX_MEASUREMENT_RANGE距离以内的点用于定位
static void scan_callback(const sensor_msgs::PointCloud2::ConstPtr& input)
{
  pcl::PointCloud<pcl::PointXYZ> scan;
  pcl::fromROSMsg(*input, scan);
  scan = removePointsByRange(scan, 0, MAX_MEASUREMENT_RANGE);

  pcl::PointCloud<pcl::PointXYZ>::Ptr scan_ptr(new pcl::PointCloud<pcl::PointXYZ>(scan));//重新赋值给scan_ptr
  pcl::PointCloud<pcl::PointXYZ>::Ptr filtered_scan_ptr(new pcl::PointCloud<pcl::PointXYZ>());

  sensor_msgs::PointCloud2 filtered_msg;

  // if voxel_leaf_size < 0.1 voxel_grid_filter cannot down sample (It is specification in PCL)
  if (voxel_leaf_size >= 0.1)
  {
    // Downsampling the velodyne scan using VoxelGrid filter
    pcl::VoxelGrid<pcl::PointXYZ> voxel_grid_filter;
    voxel_grid_filter.setLeafSize(voxel_leaf_size, voxel_leaf_size, voxel_leaf_size);
    voxel_grid_filter.setInputCloud(scan_ptr);//设置输入点云
    voxel_grid_filter.filter(*filtered_scan_ptr);//降采样
    pcl::toROSMsg(*filtered_scan_ptr, filtered_msg);//转为ros点云
  }
  else
  {
    pcl::toROSMsg(*scan_ptr, filtered_msg);
  }

  filtered_msg.header = input->header;
  filtered_points_pub.publish(filtered_msg);//发布滤波后点云

}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "voxel_grid_filter");

  ros::NodeHandle nh;
  ros::NodeHandle private_nh("~");

  private_nh.getParam("points_topic", POINTS_TOPIC);
  private_nh.getParam("output_log", _output_log);

  private_nh.param<double>("leaf_size", voxel_leaf_size, 2.0);
  ROS_INFO_STREAM("Voxel leaf size is: "<<voxel_leaf_size);
  if(_output_log == true){
	  char buffer[80];
	  std::time_t now = std::time(NULL);//time_t 这种类型就是用来存储从1970年到现在经过了多少秒
	  std::tm *pnow = std::localtime(&now);//年月日时分秒
	  std::strftime(buffer,80,"%Y%m%d_%H%M%S",pnow);
	  ROS_INFO_STREAM("time:"<< std::string(buffer));
	  filename = "voxel_grid_filter_" + std::string(buffer) + ".csv";
	  ofs.open(filename.c_str(), std::ios::out);
	  ofs << std::fixed << std::setprecision(9)
         << std::string(buffer) << "\n";
      ofs.flush();
      ofs.close();  
  }

  // Publishers
  filtered_points_pub = nh.advertise<sensor_msgs::PointCloud2>("/filtered_points", 10);

  // Subscribers
  ros::Subscriber scan_sub = nh.subscribe(POINTS_TOPIC, 10, scan_callback);

  ros::spin();

  return 0;
}
