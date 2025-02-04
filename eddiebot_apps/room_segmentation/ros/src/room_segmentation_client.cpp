#include <room_segmentation/room_segmentation_client.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <time.h>
#include <stdlib.h>

RoomSegmentationClient::RoomSegmentationClient()
{

}


void RoomSegmentationClient::launch()
{
    // Acquire /map topic data
    nav_msgs::OccupancyGrid::ConstPtr msg = ros::topic::waitForMessage<nav_msgs::OccupancyGrid>("/map", nh_);

    const nav_msgs::MapMetaData &map_info = msg->info;
    int height = map_info.height;
    int width = map_info.width;
    double resolution = map_info.resolution;
    geometry_msgs::Point origin_pos = map_info.origin.position;
    const std::vector<signed char> &map_data = msg->data;

    // parse map data, convert to 8-bit single channel image
    cv::Mat map_img = map_parser_(map_data, width, height);
    cv::Mat segmented_map = segment_room_(map_img, resolution, origin_pos.x, origin_pos.y);
}


// save room segmentation to file
void RoomSegmentationClient::save2json_(room_segmentation::MapSegmentationResultConstPtr result,
    double resolution, double x, double y)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
 
    writer.StartObject();
 
    writer.Key("n_rooms");
    writer.Int(result->room_information_in_pixel.size());

    writer.Key("room_centers");
    writer.StartArray();
    for(int i = 0; i < result->room_information_in_pixel.size(); i++)
    {
        writer.StartObject();
        writer.Key("x");
        writer.Int(result->room_information_in_pixel[i].room_center.x);
        writer.Key("y");
        writer.Int(result->room_information_in_pixel[i].room_center.y);        
        writer.EndObject();
    }
    writer.EndArray();

    writer.Key("boundary");
    int min_x = 99999, min_y = 99999;
    int max_x = -1, max_y = -1;
    for(int i = 0; i < result->room_information_in_pixel.size(); i++)
    {
        min_x = std::min(min_x, (int)result->room_information_in_pixel[i].room_min_max.points[0].x);
        min_y = std::min(min_y, (int)result->room_information_in_pixel[i].room_min_max.points[0].y);
        max_x = std::max(max_x, (int)result->room_information_in_pixel[i].room_min_max.points[1].x);
        max_y = std::max(max_y, (int)result->room_information_in_pixel[i].room_min_max.points[1].y);
    }
    writer.StartObject();
    writer.Key("min_x");
    writer.Int(min_x);
    writer.Key("min_y");
    writer.Int(min_y);
    writer.Key("max_x");
    writer.Int(max_x);
    writer.Key("max_y");
    writer.Int(max_y); 
    writer.EndObject();

    writer.Key("resolution");
    writer.Double(resolution);

    writer.Key("origin");
    writer.StartObject();
    writer.Key("x");
    writer.Double(x);
    writer.Key("y");
    writer.Double(y);
    writer.EndObject();

    writer.Key("map");
    writer.StartArray();
    cv_bridge::CvImagePtr cv_ptr_obj;
    cv_ptr_obj = cv_bridge::toCvCopy(result->segmented_map, sensor_msgs::image_encodings::TYPE_32SC1);
    cv::Mat segmented_map = cv_ptr_obj->image;
    for(int i = 0; i < segmented_map.rows; i++){
        writer.StartArray();
        for(int j = 0; j < segmented_map.cols; j++){
            writer.Int(segmented_map.at<int>(i, j));
        }
        writer.EndArray();
    }
    writer.EndArray();

    writer.EndObject();

    std::string output_path;
    nh_.param<std::string>("/json_output", output_path, "segmented_map.json");
    
    std::ofstream out(output_path.c_str());
    out << buffer.GetString() << std::endl;
    std::cout << "Json file is save at: " << output_path << std::endl;
}


cv::Mat RoomSegmentationClient::segment_room_(cv::Mat &img, double resolution, double x, double y)
{
    sensor_msgs::Image labeling;
    cv_bridge::CvImage cv_image;
    cv_image.encoding = "mono8";
    cv_image.image = img;
    cv_image.toImageMsg(labeling);

    // create the action client --> "name of server"
    // true causes the client to spin its own thread
    actionlib::SimpleActionClient<room_segmentation::MapSegmentationAction> ac("room_segmentation_server", true);
    ROS_INFO("Waiting for action server to start.");
    // wait for the action server to start
    ac.waitForServer(); //will wait for infinite time
    ROS_INFO("Action server started, sending goal.");

    // send a goal to the action
    room_segmentation::MapSegmentationGoal goal;
    goal.input_map = labeling;
    goal.map_origin.position.x = x;
    goal.map_origin.position.y = y;
    goal.map_resolution = resolution;
    goal.return_format_in_meter = false;
    goal.return_format_in_pixel = true;
    goal.robot_radius = 0.4;
    ac.sendGoal(goal);

    //wait for the action to return
    bool finished_before_timeout = ac.waitForResult(ros::Duration());

    if (finished_before_timeout)
    {
        ROS_INFO("Finished successfully!");
        room_segmentation::MapSegmentationResultConstPtr result_seg = ac.getResult();

        // display
        cv_bridge::CvImagePtr cv_ptr_obj;
        cv_ptr_obj = cv_bridge::toCvCopy(result_seg->segmented_map, sensor_msgs::image_encodings::TYPE_32SC1);
        cv::Mat segmented_map = cv_ptr_obj->image;
        cv::Mat colour_segmented_map = segmented_map.clone();
        colour_segmented_map.convertTo(colour_segmented_map, CV_8U);
        cv::cvtColor(colour_segmented_map, colour_segmented_map, CV_GRAY2BGR);

        // save segmented map information to a json file
        save2json_(result_seg, resolution, x, y);

        for(size_t i = 1; i <= result_seg->room_information_in_pixel.size(); ++i)
        {
            //choose random color for each room
            int blue = (rand() % 250) + 1;
            int green = (rand() % 250) + 1;
            int red = (rand() % 250) + 1;
            for(size_t u = 0; u < segmented_map.rows; ++u)
            {
                for(size_t v = 0; v < segmented_map.cols; ++v)
                {
                    if(segmented_map.at<int>(u,v) == i)
                    {
                        colour_segmented_map.at<cv::Vec3b>(u,v)[0] = blue;
                        colour_segmented_map.at<cv::Vec3b>(u,v)[1] = green;
                        colour_segmented_map.at<cv::Vec3b>(u,v)[2] = red;
                    }
                }
            }
        }
        
        //draw the room centers into the map
        for(size_t i = 0; i < result_seg->room_information_in_pixel.size(); ++i)
        {
            cv::Point current_center (result_seg->room_information_in_pixel[i].room_center.x, 
                result_seg->room_information_in_pixel[i].room_center.y);

            cv::circle(colour_segmented_map, current_center, 2, CV_RGB(0,0,255), CV_FILLED);
        }

        cv::imshow("segmented_map", colour_segmented_map);
        cv::waitKey();

        return colour_segmented_map;
    }

    return img;
}


cv::Mat RoomSegmentationClient::map_parser_(const std::vector<signed char> &data, int width, int height)
{
    cv::Mat img(height, width, CV_8UC1, cv::Scalar(0));

    for(int i = 0; i < height; i++){
        for(int j = 0; j < width; j++){
            if(data[i * width + j] == -1 || data[i * width + j] >= 90){
                img.at<unsigned char>(height-i-1, j) = 0;
            }
            else{
                img.at<unsigned char>(height-i-1, j) = 255;   
            }
        }
    }

    return img;
}


int main(int argc, char **argv)
{
    ros::init(argc, argv, "room_segmentation_client");
    srand (time(NULL));

    RoomSegmentationClient rsc;
    rsc.launch();

    return 0;
}