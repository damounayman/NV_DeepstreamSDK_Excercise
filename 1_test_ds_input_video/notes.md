
# GStreamer and DeepStream Function Summary of this example.

This program sets up a GStreamer pipeline to decode and render an H.264 video file using NVIDIA DeepStream elements.
Pipeline : filesrc -> h264parse -> nvv4l2decoder -> nvstreammux -> nvegltransform -> nveglglessink

## GStreamer Initialization and Main Loop

### GStreamer Initialization
- `gst_init(&argc, &argv);`: Initializes the GStreamer library.

### Main Loop Initialization
- `loop = g_main_loop_new(NULL, FALSE);`: Creates a new GMainLoop.

## GStreamer Elements Creation

### Pipeline and Elements
- `pipeline = gst_pipeline_new("dstest-input-video");`: Creates a GStreamer pipeline.
- `source = gst_element_factory_make("filesrc", "file-source");`: Creates a file source element.
- `h264parser = gst_element_factory_make("h264parse", "h264-parser");`: Creates an H.264 parser element.
- `decoder = gst_element_factory_make("nvv4l2decoder", "nvv4l2-decoder");`: Creates an NVIDIA hardware-accelerated video decoder element.
- `streammux = gst_element_factory_make("nvstreammux", "stream-muxer");`: Creates an NVIDIA stream multiplexer element.
- `sink = gst_element_factory_make("nveglglessink", "nvvideo-renderer");`: Creates an NVIDIA EGL sink element.

### Tegra-Specific Element
- `transform = gst_element_factory_make("nvegltransform", "nvegl-transform");`: Creates an NVIDIA EGL transform element (Tegra-specific).

## Set Up Pipeline and Elements

### Set Source Location and Streammux Properties
- `g_object_set(G_OBJECT(source), "location", argv[1], NULL);`: Sets the source location to the provided H.264 file path.
- `g_object_set(G_OBJECT(streammux), "width", MUXER_OUTPUT_WIDTH, "height", MUXER_OUTPUT_HEIGHT, "batch-size", 1, "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);`: Sets properties for the stream multiplexer.

## Bus Callback Function

### GStreamer Bus Callback
- `static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) { ... }`: Handles GStreamer bus messages, including End-of-Stream and Error messages.

## Linking Elements in the Pipeline

### Link Elements
- `gst_element_link_many(source, h264parser, decoder, NULL);`: Links source, H.264 parser, and decoder elements.
- `gst_element_request_pad_simple(streammux, pad_name_sink);`: Gets the sink pad of the stream multiplexer.
- `gst_element_get_static_pad(decoder, pad_name_src);`: Gets the source pad of the decoder.
- `gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK;`: Links the source pad of the decoder to the sink pad of the stream multiplexer.
- `gst_element_link_many(streammux, transform, sink, NULL);`: Links the remaining elements in the pipeline (Tegra-specific).
- `gst_element_link_many(streammux, sink, NULL);`: Links the remaining elements in the pipeline (non-Tegra).

## Set Pipeline to Playing State

### Set Pipeline State
- `gst_element_set_state(pipeline, GST_STATE_PLAYING);`: Sets the pipeline to the playing state.

## Main Loop Execution

### GMainLoop Execution
- `g_main_loop_run(loop);`: Starts the GMainLoop, allowing the pipeline to process video frames until encountering an error or End-of-Stream.

## Cleanup

### Stop Playback and Cleanup
- `gst_element_set_state(pipeline, GST_STATE_NULL);`: Stops the playback.
- `gst_object_unref(GST_OBJECT(pipeline));`: Deletes the pipeline.
- `g_source_remove(bus_watch_id);`: Removes the bus watch.
- `g_main_loop_unref(loop);`: Frees resources before exiting the program.

---

**Note:** This summary provides an overview of key functions used in the code. For a comprehensive understanding, refer to the inline comments and the GStreamer and DeepStream documentation.
