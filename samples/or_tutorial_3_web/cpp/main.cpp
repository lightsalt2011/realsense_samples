// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2016 Intel Corporation. All Rights Reserved.

#include <thread>
#include <librealsense/rs.hpp>

#include "rs_sdk.h"
#include <iostream>
#include "version.h"
#include "or_utils.hpp"
#include "or_console_display.hpp"
#include "or_web_display.hpp"


using namespace std;
using namespace rs::core;
using namespace rs::object_recognition;

// Version number of the samples
extern constexpr auto rs_sample_version = concat("VERSION: ",RS_SAMPLE_VERSION_STR);

std::vector<std::string> objects_names;
image_info colorInfo, depthInfo;
bool is_localize=true;
bool is_tracking=false;

// Doing the OR processing for a frame can take longer than the frame interval, so we
// keep track of whether or not we are still processing the last frame.
bool is_or_processing_frame = false;
bool is_exit = false;

unique_ptr<web_display::or_web_display>  web_view;
unique_ptr<console_display::or_console_display>    console_view;

// Use a queue to hold the sample set waiting for processing
blocking_queue<correlated_sample_set*> sample_set_queue;

// After the localization has done we track the found objects
void setTracking(const image_info& colorInfo, or_configuration_interface* or_configuration,
                 const localization_data* localization_data, const int array_size)
{
    // Change mode to tracking
    or_configuration->set_recognition_mode(rs::object_recognition::recognition_mode::TRACKING);
    // Prepare rois for the tracking
    rs::core::rect* trackingRois = new rs::core::rect[array_size];
    for (int i = 0; i< array_size; i++)
    {
        rs::core::rect roi= {localization_data[i].roi.x,localization_data[i].roi.y,
                             localization_data[i].roi.width,localization_data[i].roi.height
                            };
        trackingRois[i] =roi;
    }

    // Set the rois
    or_configuration->set_tracking_rois(trackingRois, array_size);
    delete[] trackingRois;
    status status = or_configuration->apply_changes();
    if (status != rs::core::status_no_error)
        exit(1);

    // Set the state accordingly
    is_localize = false;
    is_tracking = true;
}

// Run object localization/tracking and sending result to view
void run_object_tracking(or_video_module_impl* impl, or_data_interface* or_data,
                         or_configuration_interface* or_configuration)
{
    rs::core::status status;

    // Declare data structure and size for results
    localization_data* localization_data = nullptr;
    rs::object_recognition::tracking_data* tracking_data = nullptr;
    int array_size=0;

    correlated_sample_set* or_sample_set = nullptr;

    while(!is_exit && (or_sample_set = sample_set_queue.pop()) != NULL)
    {
        if (is_localize || is_tracking)
        {
            // Run object localization or tracking processing
            status = impl->process_sample_set(*or_sample_set);

            // Recycle sample set after processing complete
            if((*or_sample_set)[stream_type::color])
                (*or_sample_set)[stream_type::color]->release();
            if((*or_sample_set)[stream_type::depth])
                (*or_sample_set)[stream_type::depth]->release();

            (*or_sample_set)[stream_type::color] = nullptr;
            (*or_sample_set)[stream_type::depth] = nullptr;

            if (status != rs::core::status_no_error)
            {
                is_or_processing_frame = false;
                return;
            }

            if (is_localize)
            {
                // Retrieve localization data from the or_data object
                status = or_data->query_localization_result(&localization_data, array_size);
                if (status != rs::core::status_no_error)
                {
                    is_or_processing_frame = false;
                    return;
                }
            }
            else
            {
                // Retrieve tracking data from the or_data object
                status = or_data->query_tracking_result(&tracking_data, array_size);
                if (status != rs::core::status_no_error)
                {
                    is_or_processing_frame = false;
                    return;
                }
            }

            if (is_localize)
            {
                if (localization_data && array_size != 0)
                {
                    console_view->on_object_tracking_data(localization_data, tracking_data,
                                                          or_configuration, array_size, is_localize, objects_names);


                    web_view->on_object_tracking_data(localization_data, tracking_data,
                                                      or_configuration, array_size, is_localize, objects_names);
                    // After localization has done we want to track the found objects.
                    setTracking(colorInfo, or_configuration, localization_data, array_size);
                }
            }
            else
            {
                // Display the top bounding boxes with object name and probability
                if (tracking_data)
                {
                    console_view->on_object_tracking_data(localization_data, tracking_data,
                                                          or_configuration, array_size, is_localize, objects_names);
                    web_view->on_object_tracking_data(localization_data, tracking_data,
                                                      or_configuration, array_size, is_localize, objects_names);

                }
            }
        }

        is_or_processing_frame = false;
    }
}

int main(int argc,char* argv[])
{
    rs::core::status status;

    // Initialize camera and OR module
    or_utils                or_utils;
    or_video_module_impl        impl;
    or_data_interface           *or_data = nullptr;
    or_configuration_interface  *or_configuration = nullptr;

    // Start the camera
    or_utils.init_camera(colorInfo, depthInfo, impl, &or_data, &or_configuration);
    int imageWidth = or_utils.get_color_width();
    int imageHeight = or_utils.get_color_height();
    string sample_name = argv[0];

    // Change mode to localization
    or_configuration->set_recognition_mode(recognition_mode::LOCALIZATION);

    // Set the localization mechanism to use CNN
    or_configuration->set_localization_mechanism(localization_mechanism::CNN);

    // Ignore all objects under 0.7 probability (confidence)
    or_configuration->set_recognition_confidence(0.7);

    status = or_configuration->apply_changes();
    if (status != rs::core::status_no_error)
        return 1;

    // Create and start remote(Web) view
    web_view = move(web_display::make_or_web_display(sample_name, 8000, true));
    // Create console view
    console_view = move(console_display::make_console_or_display());

    vector<string> obj_name_list;
    or_utils.query_object_name_list(obj_name_list, or_configuration);
    web_view->on_object_list(obj_name_list);
    cout << endl << "-------- Press Esc key to exit --------" << endl << endl;

    // Start background thread to run recognition processing
    std::thread recognition_thread(run_object_tracking,
                                   &impl, or_data, or_configuration);
    recognition_thread.detach();

    while (!(is_exit = or_utils.user_request_exit()))
    {
        // Get the sample set from the camera
        rs::core::correlated_sample_set* sample_set = or_utils.get_sample_set(colorInfo,depthInfo);

        // The color frames and the or data are sent asynchronously, independent of
        // each other to the ui, because the or processing may take longer than 1 frame.
        if(!is_or_processing_frame)         // If we aren't already processing or for a frame
        {
            is_or_processing_frame = true;

            // Increase image reference to hold for library processing
            (*sample_set)[rs::core::stream_type::color]->add_ref();
            (*sample_set)[rs::core::stream_type::depth]->add_ref();
            // Push the sample set to queue
            sample_set_queue.push(sample_set);
        }

        // Display color image
        auto colorImage = (*sample_set)[rs::core::stream_type::color];
        // Sending dummy time stamp of 10.
        web_view->on_rgb_frame(10, imageWidth, imageHeight, colorImage->query_data());
    }

    // Stop the camera
    or_utils.stop_camera();
    cout << "-------- Stopping --------" << endl;

    return 0;
}
