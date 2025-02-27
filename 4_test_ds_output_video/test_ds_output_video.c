/*
 * Copyright (c) 2018-2021, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <math.h>

/* The muxer output resolution must be set if the input streams will be of
 * different resolution. The muxer will scale all the input frames to this
 * resolution. */
#define MUXER_OUTPUT_WIDTH 1920
#define MUXER_OUTPUT_HEIGHT 1080

/* Muxer batch formation timeout, for e.g. 40 millisec. Should ideally be set
 * based on the fastest source's framerate. */
#define MUXER_BATCH_TIMEOUT_USEC 4000000

/* The multi-stream tiler output resolution */
#define TILED_OUTPUT_WIDTH 1280
#define TILED_OUTPUT_HEIGHT 720

/* NVIDIA Decoder source pad memory feature. This feature signifies that source
 * pads having this capability will push GstBuffers containing cuda buffers. */
#define GST_CAPS_FEATURES_NVMM "memory:NVMM"

/* Callback function to receive messages from components
 * in the pipeline. */
static gboolean
bus_call(GstBus * bus, GstMessage * msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *) data;
    switch (GST_MESSAGE_TYPE (msg)) {
        case GST_MESSAGE_EOS:
            g_print ("End of stream\n");
            g_main_loop_quit (loop);
            break;
        case GST_MESSAGE_ERROR:{
            gchar *debug;
            GError *error;
            gst_message_parse_error (msg, &error, &debug);
            g_printerr ("ERROR from element %s: %s\n", \
                GST_OBJECT_NAME (msg->src), error->message);
            if (debug)
                g_printerr ("Error details: %s\n", debug);
            g_free (debug);
            g_error_free (error);
            g_main_loop_quit (loop);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

/* Callback function to check if it can use NVDEC. */
static void
cb_newpad (GstElement * decodebin, GstPad * decoder_src_pad, gpointer data)
{
    g_print ("In cb_newpad\n");
    GstCaps *caps = gst_pad_get_current_caps (decoder_src_pad);
    const GstStructure *str = gst_caps_get_structure (caps, 0);
    const gchar *name = gst_structure_get_name (str);
    GstElement *source_bin = (GstElement *) data;
    GstCapsFeatures *features = gst_caps_get_features (caps, 0);

    /* Need to check if the pad created by the decodebin is for video and not audio. */
    if (!strncmp (name, "video", 5)) {
        /* Link the decodebin pad only if decodebin has picked nvidia
         * decoder plugin nvdec_*. We do this by checking if the pad caps contain
         * NVMM memory features. */
        if (gst_caps_features_contains (features, GST_CAPS_FEATURES_NVMM)) {
            /* Get the source bin ghost pad */
            GstPad *bin_ghost_pad = gst_element_get_static_pad (source_bin, "src");
            if (!gst_ghost_pad_set_target (GST_GHOST_PAD (bin_ghost_pad), decoder_src_pad)) {
                g_printerr ("Failed to link decoder src pad to source bin ghost pad\n");
            }
            gst_object_unref (bin_ghost_pad);
        } else {
            g_printerr ("Error: Decodebin did not pick nvidia decoder plugin.\n");
        }
    }
}

/* Callback function to check if it can use NVDEC. */
static void
decodebin_child_added (GstChildProxy * child_proxy, GObject * object,
    gchar * name, gpointer user_data)
{
    g_print ("Decodebin child added: %s\n", name);
    if (g_strrstr (name, "decodebin") == name) {
        g_signal_connect (G_OBJECT (object), "child-added", G_CALLBACK (decodebin_child_added), user_data);
    }

    /* You can set config for each child decoder */
}

/* Source creation function */
static GstElement *
create_source_bin (guint index, gchar * uri)
{
    GstElement *bin = NULL, *uri_decode_bin = NULL;
    gchar bin_name[16] = { };

    g_snprintf (bin_name, 15, "source-bin-%02d", index);
    /* Create a source GstBin to abstract this bin's content from the rest of the pipeline */
    bin = gst_bin_new (bin_name);

    /* Source element for reading from the uri.
     * We will use decodebin and let it figure out the container format of the stream
     * and the codec and plug the appropriate demux and decode plugins. */
    uri_decode_bin = gst_element_factory_make ("uridecodebin", "uri-decode-bin");

    if(!bin || !uri_decode_bin) {
        g_printerr ("One element in source bin could not be created.\n");
        return NULL;
    }

    /* We set the input uri to the source element */
    g_object_set (G_OBJECT (uri_decode_bin), "uri", uri, NULL);

    /* Connect to the "pad-added" signal of the decodebin which generates a
     * callback once a new pad for raw data has beed created by the decodebin */
    g_signal_connect (G_OBJECT (uri_decode_bin), "pad-added", G_CALLBACK (cb_newpad), bin);
    g_signal_connect (G_OBJECT (uri_decode_bin), "child-added", G_CALLBACK (decodebin_child_added), bin);

    gst_bin_add (GST_BIN (bin), uri_decode_bin);

    /* We need to create a ghost pad for the source bin which will act as a proxy
     * for the video decoder src pad. The ghost pad will not have a target right
     * now. Once the decode bin creates the video decoder and generates the
     * cb_newpad callback, we will set the ghost pad target to the video decoder
     * src pad. */
    if(!gst_element_add_pad (bin, gst_ghost_pad_new_no_target ("src", GST_PAD_SRC)))
    {
        g_printerr ("Failed to add ghost pad in source bin\n");
        return NULL;
    }

    return bin;
}

/* Main function to execute pipeline */
int main (int argc, char *argv[])
{
    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL, *streammux = NULL, *tiler = NULL,
        *nvvidconv = NULL, *cap_filter = NULL, *encoder = NULL, *codecparse = NULL, *mux = NULL,  *sink = NULL;
    GstCaps *caps = NULL;
    GstBus *bus = NULL;
    guint bus_watch_id;
    GstPad *tiler_src_pad = NULL;
    guint i, num_sources;
    guint tiler_rows, tiler_columns;
    guint pgie_batch_size;

    /* Check input arguments */
    if (argc < 2) {
        g_printerr ("Usage: %s <uri1> [uri2] ... [uriN] \n", argv[0]);
        return -1;
    }
    num_sources = argc - 1;

    /* 1. Standard GStreamer initialization */
    gst_init (&argc, &argv);
    loop = g_main_loop_new (NULL, FALSE);

    /* 2. Create gstreamer elements */
    /* Create Pipeline element that will form a connection of other elements */
    pipeline = gst_pipeline_new ("dstest-output-video");

    /* Create nvstreammux instance to form batches from one or more sources. */
    streammux = gst_element_factory_make ("nvstreammux", "stream-muxer");

    if (!pipeline || !streammux) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }
    gst_bin_add (GST_BIN (pipeline), streammux);

    /* Create source bins from single or multiple uri */
    for (i = 0; i < num_sources; i++) {
        GstPad *sinkpad, *srcpad;
        gchar pad_name[16] = { };
        GstElement *source_bin = create_source_bin (i, argv[i + 1]);

        if (!source_bin) {
            g_printerr ("Failed to create source bin. Exiting.\n");
            return -1;
        }

        gst_bin_add (GST_BIN (pipeline), source_bin);

        g_snprintf (pad_name, 15, "sink_%u", i);
        sinkpad = gst_element_request_pad_simple (streammux, pad_name);
        if (!sinkpad) {
            g_printerr ("Streammux request sink pad failed. Exiting.\n");
            return -1;
        }

        srcpad = gst_element_get_static_pad (source_bin, "src");
        if (!srcpad) {
            g_printerr ("Failed to get src pad of source bin. Exiting.\n");
            return -1;
        }

        if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK) {
            g_printerr ("Failed to link source bin to stream muxer. Exiting.\n");
            return -1;
        }

        gst_object_unref (srcpad);
        gst_object_unref (sinkpad);
    }

    /* Use nvtiler to composite the batched frames into a 2D tiled array based
     * on the source of the frames. */
    tiler = gst_element_factory_make ("nvmultistreamtiler", "nvtiler");

    /* nvvideoconvert element to convert incoming raw buffers to NVMM Mem (NvBufSurface API) */
    nvvidconv = gst_element_factory_make ("nvvideoconvert", "transform");
    /* capsfilter for nvvidconv_src */
    cap_filter = gst_element_factory_make ("capsfilter", "filter");

    /* nvv4l2h264enc element to encode by using NVENC HW */
    encoder = gst_element_factory_make ("nvv4l2h264enc", "h264-encoder");

    /* h264parser element for getting h264 header format */
    codecparse = gst_element_factory_make ("h264parse", "h264-parser");

    /* qtmux element */
    mux = gst_element_factory_make ("qtmux", "mux");

    /* filesink element */
    sink = gst_element_factory_make ("filesink", "filesink");
    if ( !tiler || !nvvidconv || !cap_filter || !encoder || !codecparse || !mux || !sink ) {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    /* 3. Set up objects */
    /* Streammux setting */
    g_object_set (G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH, "height", MUXER_OUTPUT_HEIGHT,
        "batch-size", num_sources, "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);

    /* Tiler setting */
    tiler_rows = (guint) sqrt (num_sources);
    tiler_columns = (guint) ceil (1.0 * num_sources / tiler_rows);
    g_object_set (G_OBJECT (tiler), "rows", tiler_rows, "columns", tiler_columns,
        "width", TILED_OUTPUT_WIDTH, "height", TILED_OUTPUT_HEIGHT, NULL);

    /* cap_filter setting */
    caps = gst_caps_from_string ("video/x-raw(memory:NVMM), format=I420");
    g_object_set (G_OBJECT (cap_filter), "caps", caps, NULL);

    /* Encoder setting */
    g_object_set (G_OBJECT (encoder), "bitrate", 4000000, NULL);
    g_object_set (G_OBJECT (encoder), "bufapi-version", 1, NULL);

    /* Filesink setting */
    g_object_set (G_OBJECT (sink), "location", "/workspace/out.mp4", NULL);


    /* 4. Set up the pipeline */
    /* Add a message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
    gst_object_unref (bus);

    /* Add all elements into the pipeline & Link the elements together */
    gst_bin_add_many (GST_BIN (pipeline), tiler, nvvidconv, cap_filter, encoder, codecparse, mux, sink, NULL);
    /* Link the elements together
     * nvstreammux -> nvtiler -> nvvidconv -> cap_filter -> encoder -> codecparser -> mux -> filesink */
    if (!gst_element_link_many (streammux, tiler, nvvidconv, cap_filter, encoder, codecparse, mux, sink, NULL)) {
        g_printerr ("Elements could not be linked. Exiting.\n");
        return -1;
    }

    gboolean ret = TRUE;

    /* Set the pipeline to "playing" state */
    g_print ("Now playing:");
    for (i = 0; i < num_sources; i++) {
        g_print (" %s,", argv[i + 1]);
    }
    g_print ("\n");
    gst_element_set_state (pipeline, GST_STATE_PLAYING);

    /* Wait till pipeline encounters an error or EOS */
    g_print ("Running...\n");
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
                  GST_DEBUG_GRAPH_SHOW_ALL, "ds-app-playing");
    g_main_loop_run (loop);

    /* Out of the main loop, clean up nicely */
    g_print ("Returned, stopping playback\n");
    gst_element_set_state (pipeline, GST_STATE_NULL);
    g_print ("Deleting pipeline\n");
    gst_object_unref (GST_OBJECT (pipeline));
    g_source_remove (bus_watch_id);
    g_main_loop_unref (loop);
    return 0;
}
