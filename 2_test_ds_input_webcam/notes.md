
## This C code is a GStreamer-based program for capturing video from a V4L2 (Video for Linux 2) device, performing some processing, and displaying the result.


Pipeline : v4l2src -> capsfilter -> nvvideoconvert -> capsfilter -> nvstreammux -> nvegltransform -> nveglglessink
