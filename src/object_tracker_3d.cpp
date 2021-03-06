// Jarvis Schultz
// September 13, 2011

//---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------
// This is a new node for tracking a suspended object.  It is very
// similar to nu_objecttracker.cpp.  It should be launched with a
// launch file just like in robot_tracker.cpp.  

//---------------------------------------------------------------------------
// Includes
//---------------------------------------------------------------------------

#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>

#include <iostream>
#include <sstream>
#include <fstream>

#include <ros/ros.h>
#include <Eigen/Dense>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <tf/transform_datatypes.h>
#include <puppeteer_msgs/PointPlus.h>
#include <puppeteer_msgs/speed_command.h>
#include <geometry_msgs/Point.h>

#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/features/feature.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/features/normal_3d.h>
#include <pcl/pcl_base.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>

#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/point_cloud_conversion.h>

//---------------------------------------------------------------------------
// Global Variables
//---------------------------------------------------------------------------
#define POINT_THRESHOLD (5)
typedef pcl::PointXYZ PointT;
std::string filename;

//---------------------------------------------------------------------------
// Objects and Functions
//---------------------------------------------------------------------------

class ObjectTracker
{

private:
    ros::NodeHandle n_;
    ros::Subscriber cloud_sub;
    ros::Publisher cloud_pub[2];
    ros::Publisher pointplus_pub;
    float xpos_last, xpos_lastlast;
    float ypos_last, ypos_lastlast;
    float zpos_last, zpos_lastlast;
    bool locate;
    puppeteer_msgs::speed_command srv;
    Eigen::Affine3f const_transform;
    tf::Transform tf;
    ros::Time t_now_timer, t_last_timer;
    float dt_timer;
    Eigen::VectorXf frame_limits;

public:
    ObjectTracker()
	{
	    // get filter limits:
	    get_frame_limits(filename);

	    cloud_sub = n_.subscribe("/camera/depth/points", 1,
				     &ObjectTracker::cloudcb, this);
	    pointplus_pub = n_.advertise<puppeteer_msgs::PointPlus>
		("object1_position", 100);
	    cloud_pub[0] = n_.advertise<sensor_msgs::PointCloud2>
		("object1_cloud", 1);
	    cloud_pub[1] = n_.advertise<sensor_msgs::PointCloud2>
		("object1_filtered_cloud", 1);
  
	    xpos_last = 0.0;
	    ypos_last = 0.0;
	    zpos_last = 0.0;

	    locate = true;
	    tf::StampedTransform t;
	    tf::TransformListener listener;

	    try
	    {
		ros::Time now=ros::Time::now();
		listener.waitForTransform("/camera_depth_optical_frame",
					 "/oriented_optimization_frame",
					  now, ros::Duration(1.0));
		listener.lookupTransform("/oriented_optimization_frame",
					 "/camera_depth_optical_frame",
					 ros::Time(0), t);
		tf = t;	    

	    }
	    catch (tf::TransformException ex)
	    {
		ROS_ERROR("%s", ex.what());
		ros::shutdown();
	    }

	    t_now_timer = ros::Time::now();
	    dt_timer = 0.0;
	}

    // this function gets called every time new pcl data comes in
    void cloudcb(const sensor_msgs::PointCloud2ConstPtr &scan)
	{
	    static unsigned int found_flag = 0;
	    Eigen::Vector4f centroid;
	    float xpos = 0.0;
	    float ypos = 0.0;
	    float zpos = 0.0;
	    float D_sphere = 0.05; //meters
	    float R_search = 2.0*D_sphere;
	    puppeteer_msgs::PointPlus pointplus;
	    geometry_msgs::Point point;
	    pcl::PassThrough<pcl::PointXYZ> pass;
	    Eigen::VectorXf lims(6);

	    sensor_msgs::PointCloud2::Ptr
		robot_cloud (new sensor_msgs::PointCloud2 ()),
		robot_cloud_filtered (new sensor_msgs::PointCloud2 ());    

	    pcl::PointCloud<pcl::PointXYZ>::Ptr
		cloud (new pcl::PointCloud<pcl::PointXYZ> ()),
		cloud_filtered (new pcl::PointCloud<pcl::PointXYZ> ());

	    // set a parameter telling the world that I am tracking the mass
	    ros::param::set("/tracking_mass", true);

	    // New sensor message for holding the transformed data
	    sensor_msgs::PointCloud2::Ptr scan_transformed
		(new sensor_msgs::PointCloud2());
	    try{
		pcl_ros::transformPointCloud("/oriented_optimization_frame",
					     tf, *scan, *scan_transformed);
	    }
	    catch(tf::TransformException ex)
	    {
		ROS_ERROR("%s", ex.what());
	    }
	    scan_transformed->header.frame_id = "/oriented_optimization_frame";

	    // Convert to pcl
	    pcl::fromROSMsg(*scan_transformed, *cloud);

	    // set time stamp and frame id
	    ros::Time tstamp = ros::Time::now();
	    pointplus.header.stamp = tstamp;
	    pointplus.header.frame_id = "/oriented_optimization_frame";

	    // do we need to find the object?
	    if (locate == true)
	    {
		found_flag = 0;
	    	lims << frame_limits;
	    	pass_through(cloud, cloud_filtered, lims);
		
	    	pcl::compute3DCentroid(*cloud_filtered, centroid);
	    	xpos = centroid(0); ypos = centroid(1); zpos = centroid(2);

	    	// Publish cloud:
	    	pcl::toROSMsg(*cloud_filtered, *robot_cloud_filtered);
	    	robot_cloud_filtered->header.frame_id =
		    "/oriented_optimization_frame";
	    	cloud_pub[1].publish(robot_cloud_filtered);
		
	    	// are there enough points in the point cloud?
	    	if(cloud_filtered->points.size() > POINT_THRESHOLD)
	    	{
		    found_flag++;
	    	    locate = false;  // We have re-found the object!

	    	    // set values to publish
	    	    pointplus.x = xpos; pointplus.y = ypos; pointplus.z = zpos;
	    	    pointplus.error = false;
	    	    pointplus_pub.publish(pointplus);
	    	}
	    	// otherwise we should publish a blank centroid
	    	// position with an error flag
	    	else
	    	{
	    	    // set values to publish
	    	    pointplus.x = 0.0;
	    	    pointplus.y = 0.0;
	    	    pointplus.z = 0.0;
	    	    pointplus.error = true;

	    	    pointplus_pub.publish(pointplus);	    
	    	}
	    }
	    // if "else", we are just going to calculate the centroid
	    // of the input cloud
	    else
	    {
		if (found_flag == 1)
		{
		    lims <<
			xpos_last-R_search, xpos_last+R_search,
			ypos_last-R_search, ypos_last+R_search,
			zpos_last-R_search, zpos_last+R_search;
		    found_flag++;		       
		}
		else
		{
		    // calculate velocities:
		    dt_timer = (t_now_timer.toSec()-t_last_timer.toSec());
		    ROS_DEBUG("dt_timer: %f", dt_timer);

		    float xdot = (xpos_last-xpos_lastlast)/dt_timer;
		    float ydot = (ypos_last-ypos_lastlast)/dt_timer;
		    float zdot = (zpos_last-zpos_lastlast)/dt_timer;
		    float dt = ((ros::Time::now()).toSec()-t_now_timer.toSec());
		    
		    float xsearch = xpos_last + xdot*dt;
		    float ysearch = ypos_last + ydot*dt;
		    float zsearch = zpos_last + zdot*dt;

		    lims <<
			xsearch-R_search, xsearch+R_search,
			ysearch-R_search, ysearch+R_search,
			zsearch-R_search, zsearch+R_search;
		}

		pass_through(cloud, cloud_filtered, lims);
		
	    	// are there enough points in the point cloud?
	    	if(cloud_filtered->points.size() < POINT_THRESHOLD)
	    	{
	    	    locate = true;
	    	    ROS_WARN("Lost Object at: x = %f  y = %f  z = %f\n",
	    		     xpos_last,ypos_last,zpos_last);
	  
	    	    pointplus.x = 0.0;
	    	    pointplus.y = 0.0;
	    	    pointplus.z = 0.0;
	    	    pointplus.error = true;

	    	    pointplus_pub.publish(pointplus);
		    	  	  
	    	    return;
	    	}
	
	    	pcl::compute3DCentroid(*cloud_filtered, centroid);
	    	xpos = centroid(0); ypos = centroid(1); zpos = centroid(2);

	    	pcl::toROSMsg(*cloud_filtered, *robot_cloud);
	    	robot_cloud_filtered->header.frame_id =
		    "/oriented_optimization_frame";
	    	cloud_pub[0].publish(robot_cloud);

	    	tf::Transform transform;
	    	transform.setOrigin(tf::Vector3(xpos, ypos, zpos));
	    	transform.setRotation(tf::Quaternion(0, 0, 0, 1));

	    	static tf::TransformBroadcaster br;
	    	br.sendTransform(tf::StampedTransform
	    			 (transform,ros::Time::now(),
	    			  "/oriented_optimization_frame",
				  "/object_kinect_frame"));

	    	// set pointplus message values and publish
	    	pointplus.x = xpos;
	    	pointplus.y = ypos;
	    	pointplus.z = zpos;
	    	pointplus.error = false;

	    	pointplus_pub.publish(pointplus);
	    }

	    t_last_timer = t_now_timer;
	    t_now_timer = ros::Time::now();
	    xpos_lastlast = xpos_last;
	    ypos_lastlast = ypos_last;
	    zpos_lastlast = zpos_last;
	    xpos_last = xpos;
	    ypos_last = ypos;
	    zpos_last = zpos;
	}

    void pass_through(const pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_in,
		      pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_out,
		      const Eigen::VectorXf lims)
	{
	    pcl::PointCloud<pcl::PointXYZ>::Ptr
		cloud_filtered_x (new pcl::PointCloud<pcl::PointXYZ> ()),
		cloud_filtered_y (new pcl::PointCloud<pcl::PointXYZ> ());
	    pcl::PassThrough<pcl::PointXYZ> pass;

	    if(lims.size() != 6)
	    {
		ROS_WARN("Limits for pass-through wrong size!");
		return;
	    }

	    pass.setInputCloud(cloud_in);
	    pass.setFilterFieldName("x");
	    pass.setFilterLimits(lims(0), lims(1));
	    pass.filter(*cloud_filtered_x);
	    
	    pass.setInputCloud(cloud_filtered_x);
	    pass.setFilterFieldName("y");
	    pass.setFilterLimits(lims(2), lims(3));
	    pass.filter(*cloud_filtered_y);
	    
	    pass.setInputCloud(cloud_filtered_y);
	    pass.setFilterFieldName("z");
	    pass.setFilterLimits(lims(4), lims(5));
	    pass.filter(*cloud_out);

	    return;
	}


    void get_frame_limits(std::string f)
	{
	    // first define the size of of the limits vector:
	    frame_limits.resize(6);

	    // open the file
	    std::ifstream file;
	    std::string line;
	    float tmp;
	    file.open(f.c_str(), std::fstream::in);
	    for (int i=0; i<6; i++)
	    {
		getline(file, line);
		tmp = atof(line.c_str());
		frame_limits(i) = tmp;
	    }
	    file.close();

	    return;
	}
		        
};


//---------------------------------------------------------------------------
// Main
//---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    // get the filename:
    std::string working_dir;
    working_dir = argv[0];
    std::size_t found = working_dir.find("bin");
    std::string tmp_dir = working_dir.substr(0, found);
    working_dir = tmp_dir+"launch/";
    filename = working_dir+"frame_limits.txt";

    ros::init(argc, argv, "object_tracker");
    ros::NodeHandle n;

    ROS_INFO("Starting Object Tracker...\n");
    ObjectTracker tracker;
  
    ros::spin();
  
    return 0;
}
