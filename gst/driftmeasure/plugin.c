#include <config.h>
#include <gst/gst.h>
#include "gstdriftmeasure.h"


static gboolean plugin_init(GstPlugin *plugin)
{
	gboolean ret = TRUE;
	ret = ret && gst_element_register(plugin, "driftmeasure", GST_RANK_NONE, gst_drift_measure_get_type());
	return ret;
}



GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	driftmeasure,
	"audio drift measurement element",
	plugin_init,
	VERSION,
	"LGPL",
	GST_PACKAGE_NAME,
	GST_PACKAGE_ORIGIN
)
