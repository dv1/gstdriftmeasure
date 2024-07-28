#!/usr/bin/env python3

import gi
import sys
import argparse
import traceback
import signal
import pulsectl


# Get GStreamer 1.0 packages through GObject introspection (GIR)
gi.require_version('Gst', '1.0')
from gi.repository import Gst, GObject, GLib


def msg(text, level=3):
	sys.stderr.write(('#' * level) + ' ' + text + '\n')


def error(text):
	sys.stderr.write('!!!!!! ' + text + '\n')


def print_available_pulseaudio_sources():
	pulse = pulsectl.Pulse()
	sources = pulse.source_list() or []
	num_sources = len(sources)
	sys.stderr.write('{} PulseAudio source(s) available\n\n'.format(num_sources))
	for source_nr in range(0, num_sources):
		source = sources[source_nr]
		sys.stderr.write('#{}:\n'.format(source_nr))
		sys.stderr.write('         name: "{}"\n'.format(source.name))
		sys.stderr.write('  description: "{}"\n'.format(source.description))


def signal_handler(pipeline):
	msg('Caught signal', 2)
	pipeline.push_eos_event()


class PipelineConfiguration:
	def __init__(self):
		self.source_name = ''
		self.output_csv_filename = ''
		self.output_wav_filename = ''
		self.sample_rate = 0
		self.num_channels = 0
		self.reference_channel = 0
		self.peak_threshold = 0
		self.pulse_length = 0
		self.window_size = 0


class Pipeline:
	def __init__(self, configuration, mainloop):
		msg('Setting up pipeline', 2)

		self.pipeline = Gst.Pipeline()
		self.mainloop = mainloop

		# The pipeline topology goes as follows:
		#
		# pulsesrc -> tee -> queue -> audioconvert -> driftmeasure -> filesink  (1)
		#              |
		#              +---> queue -> audioconvert -> wavenc -> filesink        (2)
		#
		# Branch (1) is always present. Branch (2) is optional; it is only present if
		# the user specified an output filename for a WAV dump of the captured data.
		#
		# The link between pulsesrc and tee is filtered to enforce a certain sample
		# rate and channel count on pulsesrc.
		#
		# Both audioconvert elements have their "dithering" property set to 'none'.
		# Dithering helps to reduce the audible impact of quantization artifacts by
		# spreading out the quantization error. However, we do not care about that
		# here, since we use artificial test signals. Instead, we must make sure that
		# the signal doesn't get noisier due to dithering. Easiest way to do that is
		# to just turn it off.
		#
		# The filesinks' async properties are set to false. "async" means that the
		# sink asynchronously switches its state, from PAUSED to PLAYING. Such an
		# asynchronous state change is useful for streams that need to play in sync
		# with the pipeline clock (audio sinks are one example). However, here, we
		# do not care about synchronized output. Furthermore, the output of the
		# driftmeasure plugin is sparse. Therefore, it makes no sense to use an
		# async PAUSED->PLAYING state change, so just turn it off.

		self.source_name = configuration.source_name

		self.pulsesrc = self.__create_element("pulsesrc", "pulsesrc")
		tee = self.__create_element("tee", "tee")

		csv_queue = self.__create_element("queue", "csv_queue")
		csv_audioconvert = self.__create_element("audioconvert", "csv_audioconvert")
		csv_driftmeasure = self.__create_element("driftmeasure", "csv_driftmeasure")
		csv_filesink = self.__create_element("filesink", "csv_filesink")

		src_caps = Gst.Caps.from_string("audio/x-raw, rate=(int){}, channels=(int){}".format(configuration.sample_rate, configuration.num_channels))

		self.pipeline.add(self.pulsesrc)
		self.pipeline.add(tee)
		self.pipeline.add(csv_queue)
		self.pipeline.add(csv_audioconvert)
		self.pipeline.add(csv_driftmeasure)
		self.pipeline.add(csv_filesink)

		self.pulsesrc.link_filtered(tee, src_caps)
		tee.link(csv_queue)
		csv_queue.link(csv_audioconvert)
		csv_audioconvert.link(csv_driftmeasure)
		csv_driftmeasure.link(csv_filesink)

		csv_audioconvert.set_property('dithering', 'none')
		csv_driftmeasure.set_property('reference-channel', configuration.reference_channel)
		csv_driftmeasure.set_property('peak-threshold', configuration.peak_threshold)
		csv_driftmeasure.set_property('pulse-length', configuration.pulse_length * 1000)
		csv_driftmeasure.set_property('window-size', configuration.window_size * 1000000)
		# We do not want any CSV output if we detect a peak in the
		# reference channel but no peaks in the other channels.
		csv_driftmeasure.set_property('omit-output-if-no-peaks', True)
		csv_filesink.set_property('location', configuration.output_csv_filename)
		csv_filesink.set_property('async', False)
		csv_filesink.set_property('buffer-mode', 'unbuffered')

		if configuration.output_wav_filename:
			wav_queue = self.__create_element("queue", "wav_queue")
			wav_audioconvert = self.__create_element("audioconvert", "wav_audioconvert")
			wav_wavenc = self.__create_element("wavenc", "wav_wavenc")
			wav_filesink = self.__create_element("filesink", "wav_filesink")

			self.pipeline.add(wav_queue)
			self.pipeline.add(wav_audioconvert)
			self.pipeline.add(wav_wavenc)
			self.pipeline.add(wav_filesink)

			tee.link(wav_queue)
			wav_queue.link(wav_audioconvert)
			wav_audioconvert.link(wav_wavenc)
			wav_wavenc.link(wav_filesink)

			wav_audioconvert.set_property('dithering', 'none')
			wav_filesink.set_property('location', configuration.output_wav_filename)
			wav_filesink.set_property('async', False)

		bus = self.pipeline.get_bus()
		bus.add_watch(GLib.PRIORITY_DEFAULT, self.__bus_watch)

		msg('Pipeline setup complete', 2)

	def __del__(self):
		self.shutdown()

	def shutdown(self):
		msg('Shutting down pipeline', 2)
		if self.pipeline:
			self.pipeline.set_state(Gst.State.NULL)
			self.pipeline = None

	def start(self):
		self.pipeline.set_state(Gst.State.PLAYING)

	def push_eos_event(self):
		self.pipeline.send_event(Gst.Event.new_eos())

	def __create_element(self, factory, name):
		element = Gst.ElementFactory.make(factory, name)
		if not element:
			raise RuntimeError('could not create element "{}" with factory "{}"'.format(name, factory))
		else:
			return element

	def __bus_watch(self, bus, message):
		if message.type == Gst.MessageType.STATE_CHANGED:
			old_state, new_state, pending_state = message.parse_state_changed()
			if message.src == self.pulsesrc:
				# The pulsesrc element's "device" property only has any effect
				# after pulsesrc's state has been set to READY. Therefore, we
				# set that property's value here, so that pulsesrc captures
				# PCM data from the specified source.
				if (old_state == Gst.State.NULL) and (new_state == Gst.State.READY):
					msg('Setting pulsesrc device')
					self.pulsesrc.set_property('device', self.source_name)
			elif message.src == self.pipeline:
				old_state_name = Gst.Element.state_get_name(old_state)
				new_state_name = Gst.Element.state_get_name(new_state)
				pending_state_name = Gst.Element.state_get_name(pending_state)

				# Dump the pipeline's topology as a Graphviz .dot file after
				# the whole pipeline completed a state change. This is useful
				# for debugging and diagnostics. The dumps will be done only
				# if the GST_DEBUG_DUMP_DOT_DIR environment variable is set.
				# Otherwise, debug_bin_to_dot_file_with_ts() is a no-op.
				dot_filename = 'statechange_old-{}_new-{}_pending-{}'.format(old_state_name, new_state_name, pending_state_name)
				Gst.debug_bin_to_dot_file_with_ts(self.pipeline, Gst.DebugGraphDetails.ALL, dot_filename)

				msg('Completed state change from {} to {}; pending: {}'.format(old_state_name, new_state_name, pending_state_name))

		elif message.type == Gst.MessageType.INFO:
			err, debug = message.parse_info()
			msg('GStreamer info: {} (debug details: {})'.format(err, debug or 'none'), 2)

		elif message.type == Gst.MessageType.WARNING:
			err, debug = message.parse_warning()
			msg('GStreamer warning: {} (debug details: {})'.format(err, debug or 'none'), 2)

		elif message.type == Gst.MessageType.ERROR:
			err, debug = message.parse_error()
			error('GStreamer error: {} (debug details: {})'.format(err, debug or 'none'))
			self.mainloop.quit()

		elif message.type == Gst.MessageType.LATENCY:
			# As the GStreamer documentation states, it is recommended
			# to call recalculate_latency() once this message arrives.
			msg('Recalculatinge pipeline latency', 2)
			self.pipeline.recalculate_latency()

		elif message.type == Gst.MessageType.EOS:
			msg('End of stream detected', 2)
			self.mainloop.quit()

		return True


# Enable multithreading support in GLib (necessary if PyGOBject is older than 3.11)
pygobject_version = gi.version_info
if (pygobject_version[0] < 3) or (pygobject_version[0] == 3 and pygobject_version[1] < 11):
	GObject.threads_init()

# Setup GStreamer
Gst.init(sys.argv)


# Parse and validate command line arguments

parser = argparse.ArgumentParser(description='')
parser.add_argument('-s', '--source-name', dest='source_name', metavar='SOURCE_NAME', type=str, action='store', default=None, help='What PulseAudio source to use for capturing')
parser.add_argument('-o', '--output-csv-filename', dest='output_csv_filename', metavar='OUTPUT_CSV_FILENAME', type=str, action='store', default=None, help='Filename to write CSV data to')
parser.add_argument('-w', '--output-wav-filename', dest='output_wav_filename', metavar='OUTPUT_WAV_FILENAME', type=str, action='store', default=None, help='Filename for WAV file to write captured data; if not specified, no WAV file will be generated')
parser.add_argument('-r', '--sample-rate', dest='sample_rate', metavar='SAMPLE_RATE', type=int, action='store', default=96000, help='Sample rate to use for capturing, in Hz')
parser.add_argument('-c', '--num-channels', dest='num_channels', metavar='NUM_CHANNELS', type=int, action='store', default=2, help='Number of channels to capture (must be at least 2)')
parser.add_argument('--reference-channel', dest='reference_channel', metavar='REFERENCE_CHANNEL', type=int, action='store', default=0, help='What channel to use as reference that pulses in other channels are compared to (valid range: 0 - num_channels-1)')
parser.add_argument('--peak-threshold', dest='peak_threshold', metavar='PEAK_THRESHOLD', type=float, action='store', default=0.6, help='Amplitude threshold below which peaks are ignored (valid range: 0.0 - 1.0)')
parser.add_argument('--pulse-length', dest='pulse_length', metavar='PULSE_LENGTH', type=int, action='store', default=2000, help='Length of the pulse whose peak shall be detected, in microseconds')
parser.add_argument('--window-size', dest='window_size', metavar='WINDOW_SIZE', type=int, action='store', default=500, help='Size of window for peak detection, in milliseconds')
parser.add_argument('--list-available-sources', dest='list_available_sources', action='store_true', help='List available PulseAudio sources that can be used for the --source-name argument')

if len(sys.argv) == 1:
	parser.print_help()
	sys.exit(1)
args = parser.parse_args()

if args.list_available_sources:
	print_available_pulseaudio_sources()
	sys.exit(0)

configuration = PipelineConfiguration()

configuration.source_name = args.source_name
if not configuration.source_name:
	error('Must specify a source name (see --source-name)')
	sys.exit(1)

configuration.output_csv_filename = args.output_csv_filename
if not configuration.output_csv_filename:
	error('Must specify an output CSV filename (see --output-csv-filename)')
	sys.exit(1)

configuration.output_wav_filename = args.output_wav_filename
if configuration.output_wav_filename:
	output_wav_filename_desc = '"{}"'.format(configuration.output_wav_filename)
else:
	output_wav_filename_desc = "<WAV output disabled>"

configuration.sample_rate = args.sample_rate
if configuration.sample_rate < 1:
	error('Invalid sample rate of {} Hz'.format(configuration.sample_rate))
	sys.exit(1)

configuration.num_channels = args.num_channels
if configuration.num_channels < 2:
	error('Invalid number of channels: {} (must be at least 2)'.format(configuration.num_channels))
	sys.exit(1)

configuration.reference_channel = args.reference_channel
if (configuration.reference_channel < 0) or (configuration.reference_channel >= configuration.num_channels):
	error('Invalid reference channel: {} (must be in the range 0 - {})'.format(configuration.reference_channel, configuration.num_channels - 1))
	sys.exit(1)

configuration.peak_threshold = args.peak_threshold
if (configuration.peak_threshold < 0.0) or (configuration.peak_threshold > 1.0):
	error('Invalid peak threshold: {} (must be in the range 0.0 - 1.0)'.format(configuration.peak_threshold))
	sys.exit(1)

configuration.pulse_length = args.pulse_length
if configuration.pulse_length < 1:
	error('Invalid pulse length of {} us (must be at least 1)'.format(configuration.pulse_length))
	sys.exit(1)

configuration.window_size = args.window_size
if configuration.window_size < 1:
	error('Invalid window size of {} ms (must be at least 1)'.format(configuration.pulse_length))
	sys.exit(1)


# Print summary of our configuration

msg('Configuration:', 2)
msg('Source name:         "{}"'.format(configuration.source_name), 1)
msg('Output CSV filename: "{}"'.format(configuration.output_csv_filename), 1)
msg('Output WAV filename: {}'.format(output_wav_filename_desc), 1)
msg('Sample rate:         {} Hz'.format(configuration.sample_rate), 1)
msg('Number of channels:  {}'.format(configuration.num_channels), 1)
msg('Reference channel:   {}'.format(configuration.reference_channel), 1)
msg('Peak threshold:      {}'.format(configuration.peak_threshold), 1)
msg('Pulse length:        {} us'.format(configuration.pulse_length), 1)
msg('Window size:         {} ms'.format(configuration.window_size), 1)


pipeline = None

try:
	# Set up the mainloop and our GStreamer pipeline

	loop = GLib.MainLoop.new(None, False)
	pipeline = Pipeline(configuration, loop)

	# Add Unix signal handlers to catch Ctrl+C keypresses and HUP/TERM
	# signals so we can perform a clean shutdown in these cases.
	# The shutdown is performed by pushing an EOS event into the pipeline.
	# This gives the pipeline the chance to finish writing any buffered
	# data and write out any necessary headers (important for WAV output
	# for example), unlike shutdown(), which would just stop the pipeline.

	GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGINT, signal_handler, pipeline)
	GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGHUP, signal_handler, pipeline)
	GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGTERM, signal_handler, pipeline)

	# Everything is set up. Start the pipeline and start the mainloop.

	pipeline.start()
	loop.run()
except KeyboardInterrupt:
	pass
except:
	traceback.print_exc()


# Shut down.

msg('Quitting', 2)

if pipeline:
	pipeline.shutdown()
Gst.deinit()
