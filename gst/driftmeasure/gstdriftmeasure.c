#include <gst/gst.h>
#include <gst/base/gstadapter.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudiofilter.h>
#include "gstdriftmeasure.h"


GST_DEBUG_CATEGORY_STATIC(drift_measure_debug);
#define GST_CAT_DEFAULT drift_measure_debug


enum
{
	PROP_0,
	PROP_WINDOW_SIZE,
	PROP_PULSE_LENGTH,
	PROP_PEAK_THRESHOLD,
	PROP_REFERENCE_CHANNEL,
	PROP_UNDETECTED_PEAK_HANDLING,
	PROP_UNDETECTED_PEAK_FILL_VALUE,
	PROP_OMIT_OUTPUT_IF_NO_PEAKS
};


#define DEFAULT_WINDOW_SIZE (GST_MSECOND * 500)
#define DEFAULT_PULSE_LENGTH (GST_USECOND * 2000)
#define DEFAULT_PEAK_THRESHOLD 0.6
#define DEFAULT_REFERENCE_CHANNEL 0
#define DEFAULT_UNDETECTED_PEAK_HANDLING GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_NO_VALUE
#define DEFAULT_UNDETECTED_PEAK_FILL_VALUE 0
#define DEFAULT_OMIT_OUTPUT_IF_NO_PEAKS FALSE
#define DEFAULT_CSV_CLOCK_TIME_TIMESTAMPS FALSE


#define CSV_CAPS "text/x-csv"


#define SINK_CAPS \
	"audio/x-raw, " \
	"format = (string) F32LE, " \
	"rate = [ 1, MAX ], " \
	"channels = [ 2, MAX ], " \
	"layout = (string) { interleaved }; "

#define SRC_CAPS \
	CSV_CAPS


static GstStaticPadTemplate static_sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(SINK_CAPS)
);


static GstStaticPadTemplate static_src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(SRC_CAPS)
);


typedef enum
{
	GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_NO_VALUE,
	GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_LAST_VALUE,
	GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_FILL_VALUE
}
GstDriftMeasureUndetectedPeakHandling;


typedef enum
{
	DRIFT_MEASUREMENT_MODE_PEAK_SEARCH,
	DRIFT_MEASUREMENT_MODE_PEAK_ANALYSIS
}
DriftMeasurementMode;


#define UNDEFINED_INDEX ((guint64)(-1))


typedef struct
{
	GstClockTime timestamp;
	GstClockTimeDiff *drifts;
}
GstDriftMeasureDataset;


struct _GstDriftMeasure
{
	GstElement parent;

	/* GObject properties */
	GstClockTime window_size;
	GstClockTime pulse_length;
	gfloat peak_threshold;
	guint reference_channel;
	GstDriftMeasureUndetectedPeakHandling undetected_peak_handling;
	GstClockTimeDiff undetected_peak_fill_value;
	gboolean omit_output_if_no_peaks;

	GstPad *sinkpad, *srcpad;

	GstSegment input_segment;

	/* If true, then the output segment was started by pushing
	 * the caps and segment downstream already. */
	gboolean output_segment_started;
	/* The CSV caps. We need these for buffer pool creation and for pushing
	 * a caps event downstream, so we keep a prepared copy around. */
	GstCaps *src_caps;

	/* Audio info converted from sink caps. */
	GstAudioInfo input_audio_info;
	/* FALSE if the input_audio_info was not set yet, TRUE otherwise. */
	gboolean input_audio_info_valid;

	/* Adapter holding the frames that we keep around for analysis. The first
	 * bytes in the adapter are from the oldest frames we currently hold in
	 * there, while the last bytes are from the newest frames. As soon as
	 * we are done with the oldest frames, we flush them out. */
	GstAdapter *frame_history;
	/* The current measurement mode. */
	DriftMeasurementMode mode;
	/* window_size translated from nanoseconds to frames. */
	gsize window_size_in_frames;
	/* Index of the frame where the peak in the reference channel was found.
	 * Once this index is found, other indices are looked for in other channels.
	 * This peak is searched in the DRIFT_MEASUREMENT_MODE_PEAK_SEARCH mode,
	 * while the peaks in other channels are look for when the mode is
	 * DRIFT_MEASUREMENT_MODE_PEAK_ANALYSIS. */
	guint64 peak_frame_index;
	/* Total number of input frames we have seen so far. We need this for
	 * generating the timestamps that we put in the CSV output. */
	guint64 total_num_input_frames_seen;
	/* pulse_length translated from nanoseconds to frames. */
	gsize pulse_length_in_frames;

	/* The dataset we produced in the previous analysis mode. We need this
	 * for handling GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_LAST_VALUE. */
	GstDriftMeasureDataset last_dataset;
	/* The dataset we currently want to fill by analysing peaks. */
	GstDriftMeasureDataset current_dataset;

	/* Buffer pool for output CSV data. Created once the sink
	 * pad gets a caps event. */
	GstBufferPool *output_buffer_pool;
};


struct _GstDriftMeasureClass
{
	GstElementClass parent_class;
};


G_DEFINE_TYPE(GstDriftMeasure, gst_drift_measure, GST_TYPE_ELEMENT)


static void gst_drift_measure_dispose(GObject *object);
static void gst_drift_measure_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec);
static void gst_drift_measure_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);

static GstStateChangeReturn gst_drift_measure_change_state(GstElement *element, GstStateChange transition);

static gboolean gst_drift_measure_sink_event(GstPad *pad, GstObject *parent, GstEvent *event);
static GstFlowReturn gst_drift_measure_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer);

static void gst_drift_measure_allocate_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset *dataset);
static void gst_drift_measure_reset_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset *dataset);
static void gst_drift_measure_free_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset *dataset);
static void gst_drift_measure_copy_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset const *from, GstDriftMeasureDataset *to);
static GstFlowReturn gst_drift_measure_push_out_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset const *dataset);

static gboolean gst_drift_measure_validate_reference_channel(GstDriftMeasure *drift_measure);
static gboolean gst_drift_measure_set_input_caps(GstDriftMeasure *drift_measure, GstCaps const *caps);
static void gst_drift_measure_find_largest_frame(GstDriftMeasure *drift_measure, gfloat const *samples, guint channel, gsize num_frames, guint64 *largest_frame_index, gfloat *largest_sample);
static guint64 gst_drift_measure_scan_for_peak(GstDriftMeasure *drift_measure, gsize num_available_frames);
static GstFlowReturn gst_drift_measure_analyze_peaks(GstDriftMeasure *drift_measure, gsize num_available_frames);
static void gst_drift_measure_recalculate_num_window_frames(GstDriftMeasure *drift_measure);
static void gst_drift_measure_reset_to_search_mode(GstDriftMeasure *drift_measure);
static void gst_drift_measure_flush(GstDriftMeasure *drift_measure);
static GstFlowReturn gst_drift_measure_process_input_buffer(GstDriftMeasure *drift_measure, GstBuffer *input_buffer);




GType gst_undetected_peak_handling_get_type(void)
{
	static GType gst_undetected_peak_handling_type = 0;

	if (!gst_undetected_peak_handling_type)
	{
		static GEnumValue undetected_peak_handling_values[] =
		{
			{ GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_NO_VALUE, "Write no value (CSV column is empty)", "no-value" },
			{ GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_LAST_VALUE, "Copy the last detected value", "last-value" },
			{ GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_FILL_VALUE, "Write a configured fill value", "fill-value" },
			{ 0, NULL, NULL },
		};

		gst_undetected_peak_handling_type = g_enum_register_static(
			"GstDriftMeasureUndetectedPeakHandling",
			undetected_peak_handling_values
		);
	}

	return gst_undetected_peak_handling_type;
}




static void gst_drift_measure_class_init(GstDriftMeasureClass *klass)
{
	GObjectClass *object_class;
	GstElementClass *element_class;

	GST_DEBUG_CATEGORY_INIT(drift_measure_debug, "driftmeasure", 0, "drift measurement using peak windows");

	object_class = G_OBJECT_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&static_sink_template));
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&static_src_template));

	object_class->dispose      = GST_DEBUG_FUNCPTR(gst_drift_measure_dispose);
	object_class->set_property = GST_DEBUG_FUNCPTR(gst_drift_measure_set_property);
	object_class->get_property = GST_DEBUG_FUNCPTR(gst_drift_measure_get_property);

	element_class->change_state = GST_DEBUG_FUNCPTR(gst_drift_measure_change_state);

	gst_element_class_set_static_metadata(
		element_class,
		"driftmeasure",
		"Filter/Analyzer/Audio",
		"Measures drift between channels using peak detection",
		"Carlos Rafael Giani <crg7475@mailbox.org>"
	);

	g_object_class_install_property(
		object_class,
		PROP_WINDOW_SIZE,
		g_param_spec_uint64(
			"window-size",
			"Window size",
			"Size of window for peak detection, in nanoseconds",
			1, G_MAXUINT64,
			DEFAULT_WINDOW_SIZE,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_PULSE_LENGTH,
		g_param_spec_uint64(
			"pulse-length",
			"Pulse length",
			"Length of the pulse whose peak shall be detected, in nanoseconds",
			1, G_MAXUINT64,
			DEFAULT_PULSE_LENGTH,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_PEAK_THRESHOLD,
		g_param_spec_float(
			"peak-threshold",
			"Peak threshold",
			"Threshold for sample values to be considered part of a peak",
			0.0, 1.0,
			DEFAULT_PEAK_THRESHOLD,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_REFERENCE_CHANNEL,
		g_param_spec_uint(
			"reference-channel",
			"Reference channel",
			"Number of channel which contains the reference pulses; valid values are 0 to (num_channels - 1)",
			0, G_MAXUINT,
			DEFAULT_REFERENCE_CHANNEL,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_UNDETECTED_PEAK_HANDLING,
		g_param_spec_enum(
			"undetected-peak-handling",
			"Undetected peak handling",
			"What to do if analysis finds no peak in a non-reference channel",
			gst_undetected_peak_handling_get_type(),
			DEFAULT_UNDETECTED_PEAK_HANDLING,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_UNDETECTED_PEAK_FILL_VALUE,
		g_param_spec_int64(
			"undetected-peak-fill-value",
			"Undetected peak fill value",
			"Value to fill in when peak was not detected (used if undetected-peak-handling is set to fill-value)",
			G_MININT64, G_MAXINT64,
			DEFAULT_UNDETECTED_PEAK_FILL_VALUE,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);

	g_object_class_install_property(
		object_class,
		PROP_OMIT_OUTPUT_IF_NO_PEAKS,
		g_param_spec_boolean(
			"omit-output-if-no-peaks",
			"Omit output if no peaks",
			"Do not output anything if analysis finds no peaks in any of the non-reference channels",
			DEFAULT_OMIT_OUTPUT_IF_NO_PEAKS,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);
}


static void gst_drift_measure_init(GstDriftMeasure *drift_measure)
{
	drift_measure->window_size = DEFAULT_WINDOW_SIZE;
	drift_measure->pulse_length = DEFAULT_PULSE_LENGTH;
	drift_measure->peak_threshold = DEFAULT_PEAK_THRESHOLD;
	drift_measure->reference_channel = DEFAULT_REFERENCE_CHANNEL;
	drift_measure->undetected_peak_handling = DEFAULT_UNDETECTED_PEAK_HANDLING;
	drift_measure->undetected_peak_fill_value = DEFAULT_UNDETECTED_PEAK_FILL_VALUE;
	drift_measure->omit_output_if_no_peaks = DEFAULT_OMIT_OUTPUT_IF_NO_PEAKS;

	drift_measure->output_segment_started = FALSE;
	drift_measure->src_caps = gst_caps_new_empty_simple(CSV_CAPS);

	gst_audio_info_init(&(drift_measure->input_audio_info));
	drift_measure->input_audio_info_valid = FALSE;

	drift_measure->frame_history = gst_adapter_new();
	drift_measure->mode = DRIFT_MEASUREMENT_MODE_PEAK_SEARCH;
	drift_measure->window_size_in_frames = 0;
	drift_measure->peak_frame_index = 0;
	drift_measure->total_num_input_frames_seen = 0;

	memset(&(drift_measure->last_dataset), 0, sizeof(GstDriftMeasureDataset));
	memset(&(drift_measure->current_dataset), 0, sizeof(GstDriftMeasureDataset));

	drift_measure->output_buffer_pool = NULL;

	drift_measure->sinkpad = gst_pad_new_from_static_template(&static_sink_template, "sink");
	gst_pad_set_event_function(drift_measure->sinkpad, GST_DEBUG_FUNCPTR(gst_drift_measure_sink_event));
	gst_pad_set_chain_function(drift_measure->sinkpad, GST_DEBUG_FUNCPTR(gst_drift_measure_chain));
	gst_element_add_pad(GST_ELEMENT(drift_measure), drift_measure->sinkpad);

	drift_measure->srcpad = gst_pad_new_from_static_template(&static_src_template, "src");
	gst_element_add_pad(GST_ELEMENT(drift_measure), drift_measure->srcpad);
}


static void gst_drift_measure_dispose(GObject *object)
{
	GstDriftMeasure *drift_measure = GST_DRIFT_MEASURE(object);

	if (drift_measure->src_caps != NULL)
	{
		gst_caps_unref(drift_measure->src_caps);
		drift_measure->src_caps = NULL;
	}

	G_OBJECT_CLASS(gst_drift_measure_parent_class)->dispose(object);
}


static void gst_drift_measure_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec)
{
	GstDriftMeasure *drift_measure = GST_DRIFT_MEASURE(object);

	switch (prop_id)
	{
		case PROP_WINDOW_SIZE:
		{
			GST_OBJECT_LOCK(object);
			drift_measure->window_size = g_value_get_uint64(value);
			if (drift_measure->input_audio_info_valid)
				gst_drift_measure_recalculate_num_window_frames(drift_measure);
			gst_drift_measure_flush(drift_measure);
			GST_OBJECT_UNLOCK(object);
			break;
		}

		case PROP_PULSE_LENGTH:
		{
			GST_OBJECT_LOCK(object);
			drift_measure->pulse_length = g_value_get_uint64(value);
			gst_drift_measure_flush(drift_measure);
			GST_OBJECT_UNLOCK(object);
			break;
		}

		case PROP_PEAK_THRESHOLD:
		{
			GST_OBJECT_LOCK(object);
			drift_measure->peak_threshold = g_value_get_float(value);
			gst_drift_measure_flush(drift_measure);
			GST_OBJECT_UNLOCK(object);
			break;
		}

		case PROP_REFERENCE_CHANNEL:
		{
			guint reference_channel;

			GST_OBJECT_LOCK(object);

			reference_channel = g_value_get_uint(value);

			if (!gst_drift_measure_validate_reference_channel(drift_measure))
				break;

			drift_measure->reference_channel = reference_channel;
			gst_drift_measure_flush(drift_measure);

			GST_OBJECT_UNLOCK(object);

			break;
		}

		case PROP_UNDETECTED_PEAK_HANDLING:
		{
			GST_OBJECT_LOCK(object);
			drift_measure->undetected_peak_handling = g_value_get_enum(value);
			gst_drift_measure_flush(drift_measure);
			GST_OBJECT_UNLOCK(object);
			break;
		}

		case PROP_UNDETECTED_PEAK_FILL_VALUE:
		{
			GST_OBJECT_LOCK(object);
			drift_measure->undetected_peak_fill_value = g_value_get_int64(value);
			GST_OBJECT_UNLOCK(object);
			break;
		}

		case PROP_OMIT_OUTPUT_IF_NO_PEAKS:
		{
			GST_OBJECT_LOCK(object);
			drift_measure->omit_output_if_no_peaks = g_value_get_boolean(value);
			GST_OBJECT_UNLOCK(object);
			break;
		}

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static void gst_drift_measure_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GstDriftMeasure *drift_measure = GST_DRIFT_MEASURE(object);

	switch (prop_id)
	{
		case PROP_WINDOW_SIZE:
			GST_OBJECT_LOCK(object);
			g_value_set_uint64(value, drift_measure->window_size);
			GST_OBJECT_UNLOCK(object);
			break;

		case PROP_PULSE_LENGTH:
			GST_OBJECT_LOCK(object);
			g_value_set_uint64(value, drift_measure->pulse_length);
			GST_OBJECT_UNLOCK(object);
			break;

		case PROP_PEAK_THRESHOLD:
			GST_OBJECT_LOCK(object);
			g_value_set_float(value, drift_measure->peak_threshold);
			GST_OBJECT_UNLOCK(object);
			break;

		case PROP_REFERENCE_CHANNEL:
			GST_OBJECT_LOCK(object);
			g_value_set_uint(value, drift_measure->reference_channel);
			GST_OBJECT_UNLOCK(object);
			break;

		case PROP_UNDETECTED_PEAK_HANDLING:
			GST_OBJECT_LOCK(object);
			g_value_set_enum(value, drift_measure->undetected_peak_handling);
			GST_OBJECT_UNLOCK(object);
			break;

		case PROP_UNDETECTED_PEAK_FILL_VALUE:
			GST_OBJECT_LOCK(object);
			g_value_set_int64(value, drift_measure->undetected_peak_fill_value);
			GST_OBJECT_UNLOCK(object);
			break;

		case PROP_OMIT_OUTPUT_IF_NO_PEAKS:
			GST_OBJECT_LOCK(object);
			g_value_set_boolean(value, drift_measure->omit_output_if_no_peaks);
			GST_OBJECT_UNLOCK(object);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static GstStateChangeReturn gst_drift_measure_change_state(GstElement *element, GstStateChange transition)
{
	GstStateChangeReturn ret;
	GstDriftMeasure *drift_measure = GST_DRIFT_MEASURE(element);

	ret = GST_ELEMENT_CLASS(gst_drift_measure_parent_class)->change_state(element, transition);
	if (ret == GST_STATE_CHANGE_FAILURE)
		return ret;

	switch (transition)
	{
		case GST_STATE_CHANGE_PAUSED_TO_READY:
		{
			GST_OBJECT_LOCK(drift_measure);

			gst_drift_measure_flush(drift_measure);

			gst_drift_measure_free_dataset(drift_measure, &(drift_measure->last_dataset));
			gst_drift_measure_free_dataset(drift_measure, &(drift_measure->current_dataset));

			drift_measure->output_segment_started = FALSE;

			GST_OBJECT_UNLOCK(drift_measure);

			break;
		}

		case GST_STATE_CHANGE_READY_TO_NULL:
		{
			if (drift_measure->output_buffer_pool != NULL)
			{
				gst_object_unref(GST_OBJECT(drift_measure->output_buffer_pool));
				drift_measure->output_buffer_pool = NULL;
			}

			g_object_unref(G_OBJECT(drift_measure->frame_history));
			drift_measure->frame_history = NULL;

			break;
		}

		default:
			break;
	}

	return ret;
}


static gboolean gst_drift_measure_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
	GstDriftMeasure *drift_measure = GST_DRIFT_MEASURE(parent);

	switch(GST_EVENT_TYPE(event))
	{
		case GST_EVENT_FLUSH_STOP:
		{
			/* Flush our history */
			GST_DEBUG_OBJECT(drift_measure, "got flush_stop event; flushing history");
			GST_OBJECT_LOCK(drift_measure);
			gst_drift_measure_flush(drift_measure);
			GST_OBJECT_UNLOCK(drift_measure);

			/* Forward the event */
			return gst_pad_push_event(drift_measure->srcpad, event);
		}

		case GST_EVENT_EOS:
		{
			/* Flush our history */
			GST_DEBUG_OBJECT(drift_measure, "got eos event; flushing history");
			gst_drift_measure_flush(drift_measure);

			/* Forward the event */
			return gst_pad_push_event(drift_measure->srcpad, event);
		}

		case GST_EVENT_CAPS:
		{
			/* We need to parse the input caps to be able to
			 * properly search the incoming PCM data for peaks. */

			GstCaps *input_caps;
			gboolean retval = TRUE;

			gst_event_parse_caps(event, &input_caps);
			g_assert(input_caps != NULL);

			GST_DEBUG_OBJECT(drift_measure, "got caps event with caps %" GST_PTR_FORMAT, (gpointer)input_caps);

			GST_OBJECT_LOCK(drift_measure);
			retval = gst_drift_measure_set_input_caps(drift_measure, input_caps);
			GST_OBJECT_UNLOCK(drift_measure);

			/* Unref the event. Do not forward it, since we do not forward
			 * the input PCM data. Instead, we output CSV data. */
			gst_event_unref(event);

			return retval;
		}

		case GST_EVENT_SEGMENT:
		{
			GstSegment const *segment;

			gst_event_parse_segment(event, &segment);

			GST_DEBUG_OBJECT(drift_measure, "got segment event: %" GST_SEGMENT_FORMAT, (gpointer)segment);

			/* We use the base field of the input segments for producing
			 * timestamps in the CSV output (not to be confused with the
			 * PTS and DTS of outgoing buffers, which we do _not_ set). */
			drift_measure->input_segment = *segment;
			gst_drift_measure_flush(drift_measure);

			/* Input segment events are never forwarded, since input and output
			 * segments never are the same. */
			gst_event_unref(event);

			return TRUE;
		}

		default:
			return gst_pad_event_default(pad, parent, event);
	}
}


static GstFlowReturn gst_drift_measure_chain(G_GNUC_UNUSED GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
	GstFlowReturn flow_ret;
	GstDriftMeasure *drift_measure = GST_DRIFT_MEASURE(parent);

	if (!drift_measure->output_segment_started)
	{
		/* If we did not start the output segment yet, do so now.
		 * To that end, push a caps event with the CSV caps inside,
		 * then push a segment event. (stream-start will have been
		 * forwarded already by GstElement at this point.) */

		GstSegment segment;

		if (!gst_pad_push_event(drift_measure->srcpad, gst_event_new_caps(drift_measure->src_caps)))
		{
			GST_ERROR_OBJECT(drift_measure, "could not push caps event downstream");
			return GST_FLOW_ERROR;
		}

		gst_segment_init(&segment, GST_FORMAT_BYTES);

		if (!gst_pad_push_event(drift_measure->srcpad, gst_event_new_segment(&segment)))
		{
			GST_ERROR_OBJECT(drift_measure, "could not push segment event downstream");
			return GST_FLOW_ERROR;
		}

		drift_measure->output_segment_started = TRUE;
	}

	/* Perform the main processing. We hold the object lock to avoid
	 * race conditions that could otherwise happen when the user sets
	 * new property values while we are processing. */
	GST_OBJECT_LOCK(drift_measure);
	flow_ret = gst_drift_measure_process_input_buffer(drift_measure, buffer);
	GST_OBJECT_UNLOCK(drift_measure);

	/* We are done with this buffer. */
	gst_buffer_unref(buffer);

	return flow_ret;
}


static void gst_drift_measure_allocate_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset *dataset)
{
	guint num_channels;

	num_channels = GST_AUDIO_INFO_CHANNELS(&(drift_measure->input_audio_info));
	g_assert(num_channels >= 2);

	dataset->drifts = g_slice_alloc(sizeof(GstClockTimeDiff) * (num_channels - 1));

	gst_drift_measure_reset_dataset(drift_measure, dataset);
}


static void gst_drift_measure_reset_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset *dataset)
{
	dataset->timestamp = GST_CLOCK_TIME_NONE;

	if (drift_measure->input_audio_info_valid)
	{
		guint num_channels, channel;

		num_channels = GST_AUDIO_INFO_CHANNELS(&(drift_measure->input_audio_info));
		g_assert(num_channels >= 2);

		if (dataset->drifts != NULL)
		{
			for (channel = 0; channel < (num_channels - 1); ++channel)
				dataset->drifts[channel] = GST_CLOCK_STIME_NONE;
		}
	}
}


static void gst_drift_measure_free_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset *dataset)
{
	if (drift_measure->input_audio_info_valid)
	{
		guint num_channels = GST_AUDIO_INFO_CHANNELS(&(drift_measure->input_audio_info));
		g_assert(num_channels >= 2);

		g_slice_free1(sizeof(GstClockTimeDiff) * (num_channels - 1), dataset->drifts);
	}

	dataset->timestamp = GST_CLOCK_TIME_NONE;
	dataset->drifts = NULL;
}


static void gst_drift_measure_copy_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset const *from, GstDriftMeasureDataset *to)
{
	guint num_channels, channel;

	num_channels = GST_AUDIO_INFO_CHANNELS(&(drift_measure->input_audio_info));
	g_assert(num_channels >= 2);

	to->timestamp = from->timestamp;
	for (channel = 0; channel < (num_channels - 1); ++channel)
		to->drifts[channel] = from->drifts[channel];
}


/* The lengths 20 and 21 refer to the length of a string representation
 * of a 64-bit integer: at most 20 digits, plus one sign character */

static gint int64_to_string(gchar *destination, gint64 integer)
{
	int num_if_buffer_big_enough = g_snprintf(destination, 21, "%" G_GINT64_FORMAT, integer);
	return MIN(num_if_buffer_big_enough, 21);
}

static gint uint64_to_string(gchar *destination, guint64 integer)
{
	int num_if_buffer_big_enough = g_snprintf(destination, 20, "%" G_GUINT64_FORMAT, integer);
	return MIN(num_if_buffer_big_enough, 20);
}


static GstFlowReturn gst_drift_measure_push_out_dataset(GstDriftMeasure *drift_measure, GstDriftMeasureDataset const *dataset)
{
	/* must be called with object lock held */

	GstBuffer *output_buffer;
	GstMapInfo map_info;
	gchar *write_pointer;
	gint num_written;
	guint num_channels, channel;
	GstFlowReturn flow_ret;
	gsize actual_size;

	num_channels = GST_AUDIO_INFO_CHANNELS(&(drift_measure->input_audio_info));
	g_assert(num_channels >= 2);

	flow_ret = gst_buffer_pool_acquire_buffer(drift_measure->output_buffer_pool, &output_buffer, NULL);
	if (flow_ret != GST_FLOW_OK)
	{
		GST_ERROR_OBJECT(drift_measure, "could not acquire output buffer: %s", gst_flow_get_name(flow_ret));
		return flow_ret;
	}

	gst_buffer_map(output_buffer, &map_info, GST_MAP_WRITE);

	write_pointer = (gchar *)(map_info.data);

	/* Write the timestamp. */
	num_written = uint64_to_string(write_pointer, dataset->timestamp);
	write_pointer += num_written;

	/* Write the drift values including their preceding comma delimiters. */
	for (channel = 0; channel < (num_channels - 1); ++channel)
	{
		GstClockTimeDiff drift = dataset->drifts[channel];

		*write_pointer++ = ',';

		if (drift != GST_CLOCK_STIME_NONE)
		{
			num_written = int64_to_string(write_pointer, drift);
			write_pointer += num_written;
		}
	}

	/* Finish the CSV row with a newline character. */
	*write_pointer++ = '\n';

	/* Note down how large the CSV data actually is
	 * (= how long the CSV line is, including its newline character). */
	actual_size = (write_pointer - (gchar *)(map_info.data));

	gst_buffer_unmap(output_buffer, &map_info);

	/* Now resize the buffer to the actual data size, which _at most_ is
	 * the maximum CSV size we computed when creating the buffer pool.
	 * Since most of the time, the actual size is less than the maximum size,
	 * we have to resize the buffer, otherwise downstream will think that
	 * the bytes beyond the first actual_size ones are also valid data. */
	gst_buffer_set_size(output_buffer, actual_size);

	GST_OBJECT_UNLOCK(drift_measure);
	flow_ret = gst_pad_push(drift_measure->srcpad, output_buffer);
	GST_OBJECT_LOCK(drift_measure);

	return flow_ret;
}


static gboolean gst_drift_measure_validate_reference_channel(GstDriftMeasure *drift_measure)
{
	/* must be called with object lock held */

	gboolean ret = TRUE;
	guint num_channels = GST_AUDIO_INFO_CHANNELS(&(drift_measure->input_audio_info));

	if (G_UNLIKELY(drift_measure->input_audio_info_valid && (drift_measure->reference_channel >= num_channels)))
	{
		GST_OBJECT_UNLOCK(drift_measure);
		GST_ELEMENT_ERROR(drift_measure, STREAM, FAILED, ("invalid reference channel"), ("reference channel %u out of bounds (valid range is 0-%u)", drift_measure->reference_channel, num_channels - 1));
		GST_OBJECT_LOCK(drift_measure);
		ret = FALSE;
	}

	return ret;
}


static gboolean gst_drift_measure_set_input_caps(GstDriftMeasure *drift_measure, GstCaps const *caps)
{
	/* must be called with object lock held */

	guint num_channels;
	GstStructure *pool_config;
	gsize max_csv_buffer_size;
	gboolean ret = TRUE;


	/* Flush present states and frame histories, since they are no longer valid. */
	gst_drift_measure_flush(drift_measure);


	/* Parse input caps */
	drift_measure->input_audio_info_valid = gst_audio_info_from_caps(&(drift_measure->input_audio_info), caps);
	if (!drift_measure->input_audio_info_valid)
	{
		GST_OBJECT_UNLOCK(drift_measure);
		GST_ELEMENT_ERROR(drift_measure, STREAM, FORMAT, ("could not use input caps"), ("caps: %" GST_PTR_FORMAT, (gpointer)caps));
		GST_OBJECT_LOCK(drift_measure);
		goto error;
	}


	/* Check if the reference channel is still valid (= it is < num_channels). */
	if (!gst_drift_measure_validate_reference_channel(drift_measure))
		goto error;


	/* Set up sizes and datasets according to our new audio info. */

	gst_drift_measure_recalculate_num_window_frames(drift_measure);

	gst_drift_measure_free_dataset(drift_measure, &(drift_measure->last_dataset));
	gst_drift_measure_free_dataset(drift_measure, &(drift_measure->current_dataset));
	gst_drift_measure_allocate_dataset(drift_measure, &(drift_measure->last_dataset));
	gst_drift_measure_allocate_dataset(drift_measure, &(drift_measure->current_dataset));

	drift_measure->pulse_length_in_frames = gst_util_uint64_scale_int_ceil(drift_measure->pulse_length, GST_AUDIO_INFO_RATE(&(drift_measure->input_audio_info)), GST_SECOND);


	/* Set up the output buffer pool. */

	num_channels = GST_AUDIO_INFO_CHANNELS(&(drift_measure->input_audio_info));

	/* Get rid of any already existing buffer pool. */
	if (drift_measure->output_buffer_pool != NULL)
		gst_object_unref(GST_OBJECT(drift_measure->output_buffer_pool));

	/* Outgoing CSV lines have the following structure:
	 *
	 * <timestamp>,<channel 1 drift>,<channel 2 drift>,<channel 3 drift>...
	 *
	 * The timestamp and drift values are 64-bit integers. Timestamps are
	 * unsigned, drift values are signed. This means that the string
	 * representation of timestamps is at most 20 characters long, and that
	 * of drift values at most 21 characters (20 digits + 1 sign).
	 *
	 * We also have 1 column for the timestamps and (num_channel-1) columns
	 * for the drift values (since we skip the reference channel).
	 *
	 * Therefore, if we factor in the comma delimiters and newline, we get:
	 *
	 * Maximum CSV line length: 20 [the timestamp] + (num_channels - 1) [the number of drift values] * (1 [the comma delimiter] + 21 [the drift value digits and a sign character]) + 1 [the newline]
	 */
	max_csv_buffer_size = 20 + (num_channels - 1) * (1 + 21) + 1;

	drift_measure->output_buffer_pool = gst_buffer_pool_new();
	pool_config = gst_buffer_pool_get_config(drift_measure->output_buffer_pool);
	gst_buffer_pool_config_set_params(pool_config, drift_measure->src_caps, max_csv_buffer_size, 0, 0);
	if (!gst_buffer_pool_set_config(drift_measure->output_buffer_pool, pool_config))
	{
		GST_ERROR_OBJECT(drift_measure, "could not set modified buffer pool configuration");
		goto error;
	}

	if (!gst_buffer_pool_set_active(drift_measure->output_buffer_pool, TRUE))
	{
		GST_ERROR_OBJECT(drift_measure, "could not activate buffer pool");
		goto error;
	}


done:
	return ret;

error:
	ret = FALSE;
	goto done;
}


static void gst_drift_measure_find_largest_frame(GstDriftMeasure *drift_measure, gfloat const *samples, guint channel, gsize num_frames, guint64 *largest_frame_index, gfloat *largest_sample)
{
	guint sample_index;
	guint64 largest_sample_index = UNDEFINED_INDEX;
	guint num_channels = GST_AUDIO_INFO_CHANNELS(&(drift_measure->input_audio_info));

	*largest_frame_index = UNDEFINED_INDEX;

	/* Search for the positive peak in the input signal. */
	/* TODO: Implement a better peak detection method. This one
	 * is susceptible to signal noise. */
	for (sample_index = channel; sample_index < (num_frames * num_channels); sample_index += num_channels)
	{
		gfloat sample = samples[sample_index];

		if (sample < drift_measure->peak_threshold)
			continue;

		if ((largest_sample_index == UNDEFINED_INDEX) || (sample > *largest_sample))
		{
			largest_sample_index = sample_index;
			*largest_sample = sample;
		}
	}

	if (largest_sample_index != UNDEFINED_INDEX)
		*largest_frame_index = largest_sample_index / num_channels;
}


static guint64 gst_drift_measure_scan_for_peak(GstDriftMeasure *drift_measure, gsize num_available_frames)
{
	/* must be called with object lock held */

	guint bytes_per_frame = GST_AUDIO_INFO_BPF(&(drift_measure->input_audio_info));
	gconstpointer mapped_ptr;
	gfloat const *samples;
	gfloat largest_sample = -G_MAXFLOAT;
	guint64 largest_frame_index = UNDEFINED_INDEX;

	g_assert(num_available_frames > 0);

	mapped_ptr = gst_adapter_map(drift_measure->frame_history, num_available_frames * bytes_per_frame);
	samples = (gfloat const *)mapped_ptr;

	gst_drift_measure_find_largest_frame(drift_measure, samples, drift_measure->reference_channel, num_available_frames, &largest_frame_index, &largest_sample);

	gst_adapter_unmap(drift_measure->frame_history);

	if (largest_frame_index != UNDEFINED_INDEX)
		GST_DEBUG_OBJECT(drift_measure, "peak detected at frame #%" G_GUINT64_FORMAT " (#%" G_GUINT64_FORMAT " in the history) with value %f", largest_frame_index + drift_measure->total_num_input_frames_seen, largest_frame_index, largest_sample);

	return largest_frame_index;
}


static GstFlowReturn gst_drift_measure_analyze_peaks(GstDriftMeasure *drift_measure, gsize num_available_frames)
{
	/* must be called with object lock held */

	guint bytes_per_frame = GST_AUDIO_INFO_BPF(&(drift_measure->input_audio_info));
	guint sample_rate = GST_AUDIO_INFO_RATE(&(drift_measure->input_audio_info));
	guint num_channels = GST_AUDIO_INFO_CHANNELS(&(drift_measure->input_audio_info));
	gconstpointer mapped_ptr;
	GstClockTime peak_frame_timestamp;
	gfloat const *samples;
	guint channel;
	guint non_ref_channel;
	gboolean found_no_peaks = TRUE;

	g_assert(num_available_frames > 0);

	/* Set the timestamp for the output dataset. */
	peak_frame_timestamp = gst_util_uint64_scale_int(drift_measure->peak_frame_index + drift_measure->total_num_input_frames_seen, GST_SECOND, sample_rate);
	if (drift_measure->input_segment.format == GST_FORMAT_TIME)
		peak_frame_timestamp += drift_measure->input_segment.base;
	drift_measure->current_dataset.timestamp = peak_frame_timestamp;

	/* Set the drift values for the output dataset. */
	mapped_ptr = gst_adapter_map(drift_measure->frame_history, num_available_frames * bytes_per_frame);
	samples = (gfloat const *)mapped_ptr;
	for (channel = 0, non_ref_channel = 0; channel < num_channels; ++channel)
	{
		gfloat largest_sample;
		guint64 largest_frame_index;

		/* Comparing the reference channel's peak against the
		 * reference channel itself makes no sense, so skip it. */
		if (channel == drift_measure->reference_channel)
			continue;

		gst_drift_measure_find_largest_frame(drift_measure, samples, channel, num_available_frames, &largest_frame_index, &largest_sample);

		if (largest_frame_index != UNDEFINED_INDEX)
		{
			/* Compute the distance from the peak we found in the current channel
			 * to the peak we found in the reference channel when we were running
			 * in the search mode earlier. This distance is the drift. */
			gint64 drift_in_frames = (gint64)largest_frame_index - (gint64)(drift_measure->peak_frame_index);
			/* Translate the drift from frames to nanoseconds.
			 * We have to do some signed integer trickery here since the
			 * gst_util_uint64_scale_int() function only accepts unsigned 64-bit
			 * integers, so we cannot pass our drift to it directly. */ 
			gint64 drift_in_nanoseconds = ((gint64)gst_util_uint64_scale_int(ABS(drift_in_frames), GST_SECOND, sample_rate)) * ((drift_in_frames < 0) ? -1 : 1);

			/* We use non_ref_channel, not channel, because non_ref_channel is
			 * incremented only after we iterated over non-reference channels,
			 * while "channels" is iterated every time, and the drifts array
			 * only contains entries for the non-reference channels. */
			drift_measure->current_dataset.drifts[non_ref_channel] = drift_in_nanoseconds;

			found_no_peaks = FALSE;

			GST_DEBUG_OBJECT(drift_measure, "channel #%u drift: %" G_GINT64_FORMAT " nanoseconds (%" G_GINT64_FORMAT " frames)", channel, drift_in_nanoseconds, drift_in_frames);
		}
		else
		{
			switch (drift_measure->undetected_peak_handling)
			{
				case GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_LAST_VALUE:
				{
					GstClockTimeDiff last_value = drift_measure->last_dataset.drifts[non_ref_channel];
					GST_DEBUG_OBJECT(drift_measure, "channel #%u pulse not found; writing last value %" G_GINT64_FORMAT " to CSV", channel, last_value);
					drift_measure->current_dataset.drifts[non_ref_channel] = (last_value == GST_CLOCK_STIME_NONE) ? drift_measure->undetected_peak_fill_value : last_value;
					break;
				}

				case GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_FILL_VALUE:
					GST_DEBUG_OBJECT(drift_measure, "channel #%u pulse not found; writing fill value %" G_GINT64_FORMAT " to CSV", channel, drift_measure->undetected_peak_fill_value);
					drift_measure->current_dataset.drifts[non_ref_channel] = drift_measure->undetected_peak_fill_value;
					break;

				case GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_NO_VALUE:
					GST_DEBUG_OBJECT(drift_measure, "channel #%u pulse not found; not writing any value to CSV (= leaving column empty)", channel);
					drift_measure->current_dataset.drifts[non_ref_channel] = GST_CLOCK_STIME_NONE;
					break;

				default:
					g_assert_not_reached();
			}
		}

		++non_ref_channel;
	}

	gst_adapter_unmap(drift_measure->frame_history);

	/* Copy the dataset we just completed. We need this if the undetected
	 * peak handling is set to GST_DRIFT_MEASURE_UNDETECTED_PEAK_HANDLING_LAST_VALUE. */
	gst_drift_measure_copy_dataset(drift_measure, &(drift_measure->current_dataset), &(drift_measure->last_dataset));

	/* Now output the completed dataset. */
	if (G_UNLIKELY(found_no_peaks && drift_measure->omit_output_if_no_peaks))
		return GST_FLOW_OK;
	else
		return gst_drift_measure_push_out_dataset(drift_measure, &(drift_measure->current_dataset));
}


static void gst_drift_measure_recalculate_num_window_frames(GstDriftMeasure *drift_measure)
{
	/* must be called with object lock held */

	guint sample_rate = GST_AUDIO_INFO_RATE(&(drift_measure->input_audio_info));
	drift_measure->window_size_in_frames = gst_util_uint64_scale_int_ceil(drift_measure->window_size, sample_rate, GST_SECOND);

	GST_INFO_OBJECT(
		drift_measure,
		"window size %" GST_TIME_FORMAT " and %u Hz sample rate => %" G_GSIZE_FORMAT " window frames",
		GST_TIME_ARGS(drift_measure->window_size),
		sample_rate,
		drift_measure->window_size_in_frames
	);
}


static void gst_drift_measure_reset_to_search_mode(GstDriftMeasure *drift_measure)
{
	/* must be called with object lock held */

	guint bytes_per_frame = GST_AUDIO_INFO_BPF(&(drift_measure->input_audio_info));
	gsize num_frames_to_flush;

	if (drift_measure->mode == DRIFT_MEASUREMENT_MODE_PEAK_SEARCH)
		return;

	/* Flush the data around the peak we last discovered. We get rid
	 * of everything before the peak plus half the pulse size to
	 * make sure this same peak is not accidentally rediscovered. */
	num_frames_to_flush = drift_measure->peak_frame_index + drift_measure->pulse_length_in_frames / 2;
	GST_DEBUG_OBJECT(drift_measure, "flushing %" G_GSIZE_FORMAT " leftover frame(s) from history", num_frames_to_flush);

	gst_adapter_flush(drift_measure->frame_history, num_frames_to_flush * bytes_per_frame);
	drift_measure->total_num_input_frames_seen += num_frames_to_flush;
	drift_measure->peak_frame_index = 0;
	drift_measure->mode = DRIFT_MEASUREMENT_MODE_PEAK_SEARCH;
}


static void gst_drift_measure_flush(GstDriftMeasure *drift_measure)
{
	/* must be called with object lock held */

	gst_adapter_clear(drift_measure->frame_history);

	gst_drift_measure_reset_dataset(drift_measure, &(drift_measure->last_dataset));
	gst_drift_measure_reset_dataset(drift_measure, &(drift_measure->current_dataset));

	drift_measure->total_num_input_frames_seen = 0;
	drift_measure->peak_frame_index = 0;
	drift_measure->mode = DRIFT_MEASUREMENT_MODE_PEAK_SEARCH;
}


static GstFlowReturn gst_drift_measure_process_input_buffer(GstDriftMeasure *drift_measure, GstBuffer *input_buffer)
{
	/* must be called with object lock held */


	GST_LOG_OBJECT(drift_measure, "processing input buffer %p", (gpointer)input_buffer);


	guint bytes_per_frame = GST_AUDIO_INFO_BPF(&(drift_measure->input_audio_info));
	gboolean loop = TRUE;
	GstFlowReturn flow_ret = GST_FLOW_OK;


	if (G_UNLIKELY(!drift_measure->input_audio_info_valid))
	{
		GST_ERROR_OBJECT(drift_measure, "cannot process input buffer since the input audio info is not valid");
		return GST_FLOW_ERROR;
	}


	gst_adapter_push(drift_measure->frame_history, gst_buffer_ref(input_buffer));
	GST_LOG_OBJECT(drift_measure, "added %" G_GUINT64_FORMAT " frames", (guint64)(gst_buffer_get_size(input_buffer) / bytes_per_frame));


	while (loop)
	{
		gsize num_available_frames = gst_adapter_available(drift_measure->frame_history) / bytes_per_frame;
		GST_LOG_OBJECT(drift_measure, "%" G_GSIZE_FORMAT " frames are in the history", num_available_frames);
		if (num_available_frames == 0)
			break;

		switch (drift_measure->mode)
		{
			case DRIFT_MEASUREMENT_MODE_PEAK_SEARCH:
			{
				guint64 peak_frame_index = gst_drift_measure_scan_for_peak(drift_measure, num_available_frames);

				if (peak_frame_index == UNDEFINED_INDEX)
				{
					if (num_available_frames >= (drift_measure->window_size_in_frames / 2))
					{
						/* There are a lot of frames, and we did not find a
						 * peak anywhere. To avoid unnecessary processing
						 * and prevent the frame history from getting too
						 * large, discard the frames that we looked at,
						 * except for the newest ones (to retain enough
						 * samples for the next search, since we need
						 * enough samples to cover the window size).*/

						gsize num_excess_frames = num_available_frames - (drift_measure->window_size_in_frames / 2);
						GST_LOG_OBJECT(drift_measure, "no peak found - discarding the oldest %" G_GSIZE_FORMAT " frames", num_excess_frames);
						gst_adapter_flush(drift_measure->frame_history, num_excess_frames * bytes_per_frame);
						drift_measure->total_num_input_frames_seen += num_excess_frames;
					}
					else
						GST_LOG_OBJECT(drift_measure, "no peak found");

					loop = FALSE;
				}
				else if (peak_frame_index < (drift_measure->window_size_in_frames / 2))
				{
					gsize num_frames_to_discard;

					/* If we detected the peak within the first half of the
					 * window size, then we cannot use it, for two reasons:
					 *
					 * #1: The "peak" may actually just be a maximum of a
					 * clipped pulse. For example, suppose that the pulse
					 * started 500 us before we started recording. Then
					 * we won't have the first 500 us of data, and therefore
					 * cannot guarantee that any peak we detect may be
					 * the "real" peak of the pulse.
					 *
					 * #2: Pulses in non-reference channels may be drifting
					 * both backwards and forwards. In order to be able to
					 * properly detect both, we must make sure that there's
					 * enough data before and after the pulse we detected. */

					num_frames_to_discard = peak_frame_index + drift_measure->pulse_length_in_frames / 2;
					num_frames_to_discard = MIN(num_frames_to_discard, num_available_frames);

					GST_DEBUG_OBJECT(drift_measure, "not enough samples in history for peak window -> ignoring peak and discarding the oldest %" G_GSIZE_FORMAT " frames", num_frames_to_discard);
					gst_adapter_flush(drift_measure->frame_history, num_frames_to_discard * bytes_per_frame);
					drift_measure->total_num_input_frames_seen += num_frames_to_discard;

					loop = FALSE;
				}
				else if ((num_available_frames - peak_frame_index) < drift_measure->pulse_length_in_frames)
				{
					/* We found a peak, but it is too close to the end of
					 * the number of available samples in the history.
					 * This is a problem, since then, the peak we found may
					 * not be the true peak - the true peak may come in the
					 * next buffer. This happens when the source records one
					 * buffer worth of data, and captures only a first part
					 * of the pulse. The next buffer will then contain the
					 * rest of the pulse. So, to make sure we do not wrongly
					 * detect the maximum sample value in the first buffer
					 * as the peak, we check how close the detected peak is
					 * to the end of the history. If the distance is smaller
					 * than the pulse length, then we cannot be sure that we
					 * really got the actual peak, and just ignore it for now.
					 * We do NOT discard any samples from the history. That
					 * way, data from the next input buffer will be appended,
					 * and we'll have a full pulse in the history that can
					 * be properly analyzed. */

					GST_DEBUG_OBJECT(drift_measure, "found a peak, but it is too close to the end of the history; ignoring it for now");
					loop = FALSE;
				}
				else
				{
					/* We found a peak, and there is enough data before
					 * the peak. Swith to analysis mode and gather data
					 * until there is also enough data _after_ the peak. */

					GST_DEBUG_OBJECT(drift_measure, "there are samples in history for peak window -> switching to analysis mode");
					drift_measure->peak_frame_index = peak_frame_index;
					drift_measure->mode = DRIFT_MEASUREMENT_MODE_PEAK_ANALYSIS;
				}

				break;
			}

			case DRIFT_MEASUREMENT_MODE_PEAK_ANALYSIS:
			{
				/* If there is enough data after the peak, we can start
				 * analyzing, because then, it is guaranteed that both
				 * before and after the peak there are enough samples
				 * for analysis to proceed. */

				if ((num_available_frames - drift_measure->peak_frame_index) >= (drift_measure->window_size_in_frames / 2))
				{
					GST_LOG_OBJECT(drift_measure, "there are now enough frames in the history for analysis");

					flow_ret = gst_drift_measure_analyze_peaks(drift_measure, num_available_frames);
					if (flow_ret < GST_FLOW_OK)
					{
						loop = FALSE;
						break;
					}

					GST_DEBUG_OBJECT(drift_measure, "peak analysis finished - switching back to search mode");
					gst_drift_measure_reset_to_search_mode(drift_measure);
				}
				else
				{
					GST_LOG_OBJECT(drift_measure, "not enough frames in the history yet for analysis");
					/* Not enough frames left in the history for further
					 * scans and analysis. Exit the loop so that we can
					 * receive more data that we can process. */
					loop = FALSE;
				}

				break;
			}

			default:
				g_assert_not_reached();
		}
	}


	GST_LOG_OBJECT(drift_measure, "input buffer %p processed", (gpointer)input_buffer);

	return flow_ret;
}
