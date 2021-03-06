/*
 * cluster_extraction.cpp
 *
 *  Created on: Apr 15, 2014
 *      Author: ace
 */

#include "cluster_extraction/cluster_extraction.h"

#include <pluginlib/class_list_macros.h>

PLUGINLIB_EXPORT_CLASS(cluster_extraction::ClusterExtraction, nodelet::Nodelet);

namespace cluster_extraction
{

ClusterExtraction::ClusterExtraction()
{

}


ClusterExtraction::~ClusterExtraction()
{
	ROS_INFO("In destructor...");

	filtered_cloud_pub.shutdown();
	table_position_pub.shutdown();
	table_cloud_pub.shutdown();
	clusters_pub.shutdown();

	if(!cloud_sub.getTopic().empty())
	{
		ROS_INFO("Un-subscribing to \'/xtion_camera/depth/points\'...");
		cloud_sub.shutdown();
	}
}

void ClusterExtraction::onInit()
{
	// nh_.setCallbackQueue(&q_);

	tf_listener = new tf::TransformListener(nh_, ros::Duration(10));
	
	filtered_cloud_pub = nh_.advertise<sensor_msgs::PointCloud2> ("/manipulation_scene", 1);

	table_position_pub = nh_.advertise<geometry_msgs::PointStamped> ("/table_position", 1);

	table_cloud_pub = nh_.advertise<sensor_msgs::PointCloud2> ("/plane_cloud", 1);

	cloud_sub = nh_.subscribe("/xtion_camera/depth/points", 3, &ClusterExtraction::cloudCallback, this);

	//table_coeffs_pub = nh.advertise<pcl_msgs::ModelCoefficients> ("/table_coeffs", 1);

	clusters_pub = nh_.advertise<doro_msgs::ClusterArray> ("/clusters", 1);

	pthread_create(&c_e_thread_id_, NULL, &ClusterExtraction::clusterExtractionThread, (void *) this);

	NODELET_INFO("Cluster Extraction has started!");

}

void* ClusterExtraction::clusterExtractionThread(void* _this_)
{
	ClusterExtraction* ptr = (ClusterExtraction *) _this_;

	ptr->subscribed_ = false;
	bool cluster_extraction_enable_param = false;
	float tolerance_param = 0.1;

	ros::param::set("/plane_extraction_tolerance", tolerance_param);

	while(ros::ok())
	{
		ros::param::get("/plane_extraction_tolerance", tolerance_param);
		ros::param::get("/cluster_extraction_enable", cluster_extraction_enable_param);
		if(cluster_extraction_enable_param)
		{
			//ptr->q_.callOne(ros::WallDuration(1.0));
			ptr->processCloud(tolerance_param);
		}
		else
		{
			//ROS_INFO(".");
			sleep(1);
		}
	}
	return NULL;
}

void ClusterExtraction::cloudCallback(const sensor_msgs::PointCloud2ConstPtr& _cloud)
{
	pcl_data = _cloud;
	//pcl::PointCloud<PoinT>::Ptr(new pcl::PointCloud<PoinT>);
	//stamp_ = _cloud->header.stamp;
	//pcl::fromROSMsg (*_cloud, *pcl_data);
}

void ClusterExtraction::processCloud(float plane_tolerance)
{
	ros::Time stamp = ros::Time::now();

	if(!pcl_data)
	{
		ROS_INFO("No xtion_camera data.");
		sleep(1);
		return;
	}

	//pcl::PointCloud<PoinT>::Ptr _cloud;// (new pcl::PointCloud<PoinT>);
	sensor_msgs::PointCloud2ConstPtr _temp_cloud_ = pcl_data;
	pcl::PointCloud<PoinT>::Ptr _cloud (new pcl::PointCloud<PoinT>);
	pcl::fromROSMsg(*_temp_cloud_, *_cloud);

	pcl::VoxelGrid<PoinT> vg_filter;
	pcl::PointCloud<PoinT>::Ptr cloud_filtered (new pcl::PointCloud<PoinT>);

	vg_filter.setInputCloud (_cloud);
	vg_filter.setLeafSize (0.01f, 0.01f, 0.01f);
	vg_filter.filter (*cloud_filtered);

	_cloud = cloud_filtered;

	/**********************************************
	 * NEW BULL
	 *********************************************/

	//////////////////////////////////////////////////////////////////////
	// Cluster Extraction
	//////////////////////////////////////////////////////////////////////
	// Findout the points that are more than 1.25 m away.
	pcl::PointIndices::Ptr out_of_range_points (new pcl::PointIndices);
	unsigned int i = 0;

	BOOST_FOREACH(const PoinT& pt, _cloud->points)
	{
		if(sqrt( (pt.x*pt.x) + (pt.y*pt.y) + (pt.z*pt.z) ) > 1.5 || isnan (pt.x) || isnan (pt.y) || isnan (pt.z) ||
				  isinf (pt.x) || isinf (pt.y) || isinf (pt.z) )
			out_of_range_points->indices.push_back(i);

		i++;

	}

	pcl::ExtractIndices<PoinT> extract;
	pcl::PointCloud<PoinT>::Ptr cloud (new pcl::PointCloud<PoinT>);

	// Perform the extraction of these points (indices).
	extract.setInputCloud(_cloud);
	extract.setIndices(out_of_range_points);
	extract.setNegative (true);
	extract.filter (*cloud);


	// Prepare plane segmentation.
	pcl::SACSegmentation<PoinT> seg;
	pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
	pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);

	pcl::PointCloud<PoinT>::Ptr cloud_plane; // This door is opened elsewhere.

	pcl::PointCloud<PoinT>::Ptr cloud_f (new pcl::PointCloud<PoinT> ());

	seg.setOptimizeCoefficients (true);
	seg.setModelType (pcl::SACMODEL_PLANE);
	seg.setMethodType (pcl::SAC_RANSAC);
	seg.setMaxIterations (1000);
	seg.setDistanceThreshold (0.02);

	// Remove the planes.
	i = 0;

	int nr_points =  (int) cloud->points.size();
	tf::StampedTransform base_link_to_openni;
	
	try
	{
		tf_listener->waitForTransform("base_link", cloud->header.frame_id, ros::Time(0), ros::Duration(1));
		//tf_listener->transformPoint("base_link", plane_normal, _plane_normal);
		tf_listener->lookupTransform("base_link", cloud->header.frame_id, ros::Time(0), base_link_to_openni);
	}
	catch(tf::TransformException& ex)
	{
	  	ROS_INFO("COCKED UP POINT INFO! Why: %s", ex.what());
	}


	// We do this here so that we can put in an empty cluster, if we see no table in the first place.
	doro_msgs::ClusterArray __clusters;

	while (cloud->points.size () > 0.5 * nr_points)
	{
		seg.setInputCloud (cloud);
		seg.segment (*inliers, *coefficients);

		if (inliers->indices.size () == 0)
		{
		  //std::cout << "Could not estimate a planar model for the given dataset." << std::endl;
		  break;
		}

		// Extract the planar inliers from the input cloud
		extract.setInputCloud (cloud);
		extract.setIndices (inliers);
		extract.setNegative (false);
		extract.filter (*cloud_f);


		// Is this a parallel to ground plane? If yes, save it.
		tf::Vector3 plane_normal (coefficients->values[0], coefficients->values[1], coefficients->values[2]);
		tf::Vector3 _plane_normal = base_link_to_openni*plane_normal;
		
		// What's the angle between this vector and the actual z axis? cos_inverse ( j component )...
		tf::Vector3 normal (_plane_normal.x(), _plane_normal.y(), _plane_normal.z());
		normal = normal.normalized();

		//std::cout<<"x: "<<normal.x()<<"\t";
		//std::cout<<"y: "<<normal.y()<<"\t";
		//std::cout<<"z: "<<normal.z()<<"\t";

		if(acos (normal.z()) < plane_tolerance)
		{
			cloud_plane = pcl::PointCloud<PoinT>::Ptr(new pcl::PointCloud<PoinT>);
			*cloud_plane = *cloud_f;
		}


		extract.setNegative (true);
		extract.filter (*cloud_f);
		*cloud = *cloud_f;
	}

	if(!cloud_plane)
	{
		ROS_INFO("No table or table-like object could be seen. Can't extract...");
		//clusters_pub.publish(__clusters);
		//sleep(1);
		return;
	}

	//ROS_INFO("Table seen.");
	//table_cloud_pub.publish(cloud_plane);

    //////////////////////////////////////////////////////////////////////
    /**
     * COMPUTE THE CENTROID OF THE PLANE AND PUBLISH IT.
     */
    //////////////////////////////////////////////////////////////////////
    Eigen::Vector4f plane_cen;

    // REMOVE COMMENTS WITH REAL ROBOT!!!
    pcl::compute3DCentroid(*cloud_plane, plane_cen);
    //std::cout<< plane_cen;

    tf::Vector3 plane_centroid (plane_cen[0], plane_cen[1], plane_cen[2]);
    tf::Vector3 _plane_centroid = base_link_to_openni*plane_centroid;
    
    geometry_msgs::PointStamped _plane_centroid_ROSMsg;
    _plane_centroid_ROSMsg.header.frame_id = "base_link";
    _plane_centroid_ROSMsg.header.stamp = stamp;
    _plane_centroid_ROSMsg.point.x = _plane_centroid.x();
    _plane_centroid_ROSMsg.point.y = _plane_centroid.y();
    _plane_centroid_ROSMsg.point.z = _plane_centroid.z();

    // Publish the centroid.
    table_position_pub.publish(_plane_centroid_ROSMsg);

    pcl::search::KdTree<PoinT>::Ptr tree (new pcl::search::KdTree<PoinT>);

   	if(cloud->points.size() == 0)
   	{
   		clusters_pub.publish(__clusters);
   		return;
   	}

   	tree->setInputCloud (cloud);

   	std::vector<pcl::PointIndices> cluster_indices;
   	pcl::EuclideanClusterExtraction<PoinT> ec;
   	ec.setClusterTolerance (0.02); // 2cm
   	ec.setMinClusterSize (20);
   	ec.setMaxClusterSize (25000);
   	ec.setSearchMethod (tree);
   	ec.setInputCloud (cloud);

   	ec.extract (cluster_indices);

   	//ROS_INFO("WIDTH: %d, HEIGHT: %d\n", _cloud->width, _cloud->height);

   	//ROS_INFO("GOOD TILL HERE!");

   	int j = 0;


   	for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin (); it != cluster_indices.end (); ++it)
   	{
   		pcl::PointCloud<PoinT>::Ptr cloud_cluster (new pcl::PointCloud<PoinT>);

		cloud_cluster->header.frame_id = cloud->header.frame_id;

   		long int color_r, color_g, color_b;
   		uint8_t mean_r, mean_g, mean_b;

   		for (std::vector<int>::const_iterator pit = it->indices.begin (); pit != it->indices.end (); pit++)
   		{
   			cloud_cluster->points.push_back (cloud->points[*pit]);
   			/* ***************** */
   			/* COLOR COMPUTATION */
   			/* ***************** */

   			color_r += cloud->points[*pit].r;
   			color_g += cloud->points[*pit].g;
   			color_b += cloud->points[*pit].b;

   		}

   		geometry_msgs::Point a, b;
   		std::vector <double> cluster_dims = getClusterDimensions(cloud_cluster, a, b);

   		mean_r = (uint8_t) (color_r / cloud_cluster->points.size ());
   		mean_g = (uint8_t) (color_g / cloud_cluster->points.size ());
   		mean_b = (uint8_t) (color_b / cloud_cluster->points.size ());

   		cloud_cluster->width = cloud_cluster->points.size ();
   		cloud_cluster->height = 1;
   		cloud_cluster->is_dense = true;
   		cloud_cluster->header.frame_id = cloud->header.frame_id;

   		Eigen::Vector4f cluster_cen;

   		pcl::compute3DCentroid(*cloud_cluster, cluster_cen);

   		tf::Vector3 cluster_centroid (cluster_cen[0], cluster_cen[1], cluster_cen[2]);
   		tf::Vector3 _cluster_centroid = base_link_to_openni*cluster_centroid;

   		geometry_msgs::PointStamped _cluster_centroid_ROSMsg;
   		_cluster_centroid_ROSMsg.header.frame_id = "base_link";
   		_cluster_centroid_ROSMsg.header.stamp = stamp;
   		_cluster_centroid_ROSMsg.point.x = _cluster_centroid.x();
  		_cluster_centroid_ROSMsg.point.y = _cluster_centroid.y();
   		_cluster_centroid_ROSMsg.point.z = _cluster_centroid.z();

   		if(DIST(cluster_centroid,plane_centroid) < 0.6 && _cluster_centroid.z() > _plane_centroid.z())
   		{
   			doro_msgs::Cluster __cluster;
   			__cluster.centroid = _cluster_centroid_ROSMsg;
   			// Push cluster dimentions. Viewed width, breadth and height
   			__cluster.cluster_size = cluster_dims;
   			__cluster.a = a;
   			__cluster.b = b;
   			// Push colors
   			__cluster.color.push_back(mean_r);
   			__cluster.color.push_back(mean_g);
   			__cluster.color.push_back(mean_b);
   			__clusters.clusters.push_back (__cluster);
   			j++;
   		}

   	}

   	clusters_pub.publish(__clusters);

   	/////////////// RUBBISH ENDS ////////////////

    //if(pcl_data)
    //	pcl_data.reset();

}

std::vector <double> ClusterExtraction::getClusterDimensions(const pcl::PointCloud<PoinT>::ConstPtr& input_cluster, geometry_msgs::Point& a, geometry_msgs::Point& b)
{
        tf::StampedTransform ptu_t_m_link_to_openni;
	
	try
	{
		tf_listener->waitForTransform("ptu_tilt_motor_link", input_cluster->header.frame_id, ros::Time(0), ros::Duration(1));
		//tf_listener->transformPoint("base_link", plane_normal, _plane_normal);
		tf_listener->lookupTransform("ptu_tilt_motor_link", input_cluster->header.frame_id, ros::Time(0), ptu_t_m_link_to_openni);
	}
	catch(tf::TransformException& ex)
	{
	  	ROS_INFO("COCKED UP POINT INFO! Why: %s", ex.what());
	}

	pcl::PointCloud <PoinT> transformed_cloud;

	BOOST_FOREACH(const PoinT& pt, input_cluster->points)
	{
		tf::Vector3 _pt_(pt.x, pt.y, pt.z);
		tf::Vector3 _pt_transformed_ = ptu_t_m_link_to_openni*_pt_;
		PoinT thisPoint;
		thisPoint.x = _pt_transformed_.x();
		thisPoint.y = _pt_transformed_.y();
		thisPoint.z = _pt_transformed_.z();
		transformed_cloud.push_back(thisPoint);
	}

	// Transformed cloud is now w.r.t. base_link frame.
	transformed_cloud.header.frame_id = "ptu_tilt_motor_link";

	double min_X = transformed_cloud.points[0].x, min_Y = transformed_cloud.points[0].y, min_Z = transformed_cloud.points[0].z,
		   max_X = transformed_cloud.points[0].x, max_Y = transformed_cloud.points[0].y, max_Z = transformed_cloud.points[0].z;

	BOOST_FOREACH(const PoinT& pt, transformed_cloud.points)
	{
		if(pt.x > max_X)
			max_X = pt.x;
		if(pt.y > max_Y)
			max_Y = pt.y;
		if(pt.z > max_Z)
			max_Z = pt.z;
		if(pt.x < min_X)
			min_X = pt.x;
		if(pt.y < min_Y)
			min_Y = pt.y;
		if(pt.z < min_Z)
			min_Z = pt.z;
	}

	b.x = min_X; b.y = min_Y; b.z = min_Z;
	a.x = max_X; a.y = max_Y, a.z = max_Z;

	std::vector <double> dims;
	// Width first.
	dims.push_back(max_Y - min_Y);
	// Breadth next.
	dims.push_back(max_X - min_X);
	// Height last.
	dims.push_back(max_Z - min_Z);

	return dims;

}

int
ClusterExtraction::getCylinders (const pcl::PointCloud<PoinT>::ConstPtr& _cloud)
{
	pcl::NormalEstimation<PoinT, pcl::Normal> ne;
	pcl::SACSegmentationFromNormals<PoinT, pcl::Normal> seg;
	pcl::search::KdTree<PoinT>::Ptr tree (new pcl::search::KdTree<PoinT> ());

	pcl::PointCloud<pcl::Normal>::Ptr cloud_norm (new pcl::PointCloud<pcl::Normal>);
	pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
	pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);

	ne.setSearchMethod (tree);
	ne.setInputCloud (_cloud);
	ne.setKSearch (50);
	ne.compute (*cloud_norm);
	//pcl::PointCloud<PoinT>::Ptr cloud_plane; // This door is opened elsewhere.

	seg.setOptimizeCoefficients (true);
	seg.setModelType (pcl::SACMODEL_CYLINDER);
	seg.setMethodType (pcl::SAC_RANSAC);
	seg.setRadiusLimits (0.01, 0.04);
	seg.setMaxIterations (10000);
	seg.setDistanceThreshold (0.03);
	seg.setNormalDistanceWeight (0.02);


	pcl::ExtractIndices<PoinT> extract;
	pcl::ExtractIndices<pcl::Normal> extract_norm;
	pcl::PointCloud<PoinT>::Ptr cloud (new pcl::PointCloud<PoinT>);
	*cloud = *_cloud;

	pcl::PointCloud<PoinT>::Ptr cloud_f (new pcl::PointCloud<PoinT>);
	pcl::PointCloud<pcl::Normal>::Ptr cloud_norm_f (new pcl::PointCloud<pcl::Normal>);

	int nr_points = (int) cloud->points.size();

	int i;

	for(i = 0;cloud->points.size () > 0.4 * nr_points; i++)
	{
		seg.setInputNormals (cloud_norm);
		seg.setInputCloud (cloud);
		seg.segment (*inliers, *coefficients);

		if (inliers->indices.size () == 0)
		{
			//std::cout << "Could not estimate a model for the given dataset." << std::endl;
			break;
		}

		// Extract the planar inliers from the input cloud
		extract.setInputCloud (cloud);
		extract.setIndices (inliers);
		extract.setNegative (true);
		extract.filter (*cloud_f);

		//ROS_INFO("%d points -> pass %d\n", cloud->points.size(), i);
		*cloud = *cloud_f;
		//ROS_INFO("%d points -> pass %d after filters...\n\n", cloud->points.size(), i);

		// Same for normals
		extract_norm.setInputCloud (cloud_norm);
		extract_norm.setIndices (inliers);
		extract_norm.setNegative (true);
		extract_norm.filter (*cloud_norm_f);

		*cloud_norm = *cloud_norm_f;

		if (inliers->indices.size () < 80)
		{
			i--;
			//printf("SACMODELS: %f\n", coefficients->values[6]);
		}
	}

	return i;

}

int
ClusterExtraction::getPlanes (const pcl::PointCloud<PoinT>::ConstPtr& _cloud)
{
	pcl::SACSegmentation<PoinT> seg;
	pcl::search::KdTree<PoinT>::Ptr tree (new pcl::search::KdTree<PoinT> ());

	pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
	pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);

	seg.setOptimizeCoefficients (true);
	seg.setModelType (pcl::SACMODEL_PLANE);
	seg.setMethodType (pcl::SAC_RANSAC);
	seg.setDistanceThreshold (0.01);

	pcl::ExtractIndices<PoinT> extract;
	pcl::PointCloud<PoinT>::Ptr cloud (new pcl::PointCloud<PoinT>);
	*cloud = *_cloud;

	pcl::PointCloud<PoinT>::Ptr cloud_f (new pcl::PointCloud<PoinT>);

	int nr_points = (int) cloud->points.size();

	int i;

	for(i = 0;cloud->points.size () > 0.4 * nr_points; i++)
	{
		seg.setInputCloud (cloud);
		seg.segment (*inliers, *coefficients);

		if (inliers->indices.size () == 0)
		{
			//std::cout << "Could not estimate a model for the given dataset." << std::endl;
			break;
		}

		// Extract the planar inliers from the input cloud
		extract.setInputCloud (cloud);
		extract.setIndices (inliers);
		extract.setNegative (true);
		extract.filter (*cloud_f);

		//ROS_INFO("%d points -> pass %d\n", cloud->points.size(), i);
		*cloud = *cloud_f;
		//ROS_INFO("%d points -> pass %d after filters...\n\n", cloud->points.size(), i);
	}

	return i;
}

} // namespace cluster_extraction
