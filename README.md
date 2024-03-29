gstdriftmeasure - GStreamer element for measuring audio synchronization drift
=============================================================================

This is an element that is useful for measuring drift between devices that
play the same signal in sync. For example, in a multiroom setup (like what
Sonos speakers do, or what AirPlay does), multiple devices are configured
to play the same audio signal in sync. However, the synchronization is never
perfect; some devices play a little sooner, some a little later. Here, we refer
to these deviations as "drift"; the receivers "drift" away from the sender
over time.

Ideally, this drift is zero. In practice, the goal is to minimize the drift as
much as possible. In order to do that, it is necessary to measure how big the
drift is, and how it develops over time. The way to measure this is to let the
sender play a special test mono signal and send it to its grouped receivers,
then record the output of all devices into one multichannel signal (one channel
per device), and feed this multichannel signal into this element. The
driftmeasure will analyze the audio stream, detect and measure drift, and
output CSV data. This data describes the drift behavior.

Measurements are typically done by using the `gst-launch-1.0` utility, and can
be performed both on a previously recorded test signal and on live recordings.

Example call that analyzes a multichannel test signal on the fly, at 96 kHz:

    gst-launch-1.0 pulsesrc ! audio/x-raw,rate=96000 ! driftmeasure ! filesink location=measured-drift.csv

This element was tested with GStreamer 1.14.4, but should work with all versions
starting at 1.4.

*IMPORTANT:* If you the `audioconvert` element comes before `driftmeasure`,
make sure that its `dithering` property is set to `none` (or 0). Otherwise,
dithering may cause inaccuracies in the measurement.

In addition, a Python 3.x script is provided, `driftmeasure-frontend.py`, for
when one just wants to do drift measurements right away. The script sets up
a GStreamer pipeline that captures PCM data from a PulseAudio source, produces
a CSV file, and optionally a WAV dump of the captured data. How to set up and
use that script is described in a section below.


Building and installing the GStreamer 1.x plugin
------------------------------------------------

This project uses [meson](https://mesonbuild.com) as its build system. Amongst other reasons, this makes
integration with existing GStreamer build setups easier, such as [Cerbero](https://gitlab.freedesktop.org/gstreamer/cerbero).

First, if you need to run this element on a different platform such as an ARM SBC,
set up [the necessary cross compilation configuration for meson](https://mesonbuild.com/Cross-compilation.html).

Then, create a build directory for an out-of-tree build:

    make build
    cd build

Now set up the build by running meson:

    meson ..

You might want to look into the `--buildtype=plain` flag if the compiler flags Meson adds are a problem.
This is particularly useful for packagers. [Read here for more.](https://mesonbuild.com/Quick-guide.html#using-meson-as-a-distro-packager)

Also, you might be interested in the `-Dprefix` and `-Dlibdir` arguments to control where to install the
resulting binaries. Again, this is particularly useful for packagers.

Finally, build and install the code by running ninja:

    ninja install

If you want to run the plugin locally, without installing it, or if you install it to a non-standard
location in your filesystem, be sure to set the `GST_PLUGIN_PATH` environment variable to point to
the location where the `libgstdriftmeasure.so` plugin is. For more information about this variable,
[read this document](https://gstreamer.freedesktop.org/data/doc/gstreamer/head/gstreamer/html/gst-running.html).


Test signal
-----------

The driftmeasure element looks for peaks. The test signal must be designed
in a way that ensure that there is exactly one peak followed by a period of
silence. This must be a mono signal.

One recommended signal structure: one sine wave period of 1000 Hz (we call
this the _signal pulse_), followed by one second of silence. Then, another
signal pulse (exactly like the previous one) appears, again followed by one
second of silence etc. This allows for detecting a drift of up to 500 ms.


Structure of a good test signal
-------------------------------

![signal structure illustration](test-signal-illustration.png)

This illustrates how a good test signal looks like. There are several
quantities that are used by the driftmeasure element:

1. The "pulse length". This is the length of the signal pulse itself. In the
   illustration, it is shown as the range covered by the red lines.

2. The "window size". This is the length of the "window" (see below). In the
   illustration, it is shown as the range covered by the green lines.

3. The "peak threshold". This is a threshold for sample values below which
   samples are ignored. It is shown in the illustration as the yellow
   horizontal line.

A good test signal must have pulses with pointed peaks. In this example,
the test signal has a peak that is one sample wide, which is ideal. Flat
peaks however are suboptimal, since the driftmeasure element then cannot detect
peaks properly, resulting in incorrect drift measurements.

It is important to know the correct window size, peak threshold, and pulse length
of the test signal. The peak threshold should be picked such that is above any
other potential local maxima, but not too close to the actual signal peak.
That way, the threshold can efficiently filter out any influence from noise
artifacts (which have low amplitude), and still let the true peak through.

Starting with version 1.14, GStreamer's `audiotestsrc` element is capable of
generating a test signal. To do that, use its "ticks" waveform. This is an example
`gst-launch-1.0` command line which generates one single-sine-period 1khz pulse
with 1 second of silence between pulses and renders it as 48 kHz S32LE mono PCM:

    gst-launch-1.0 audiotestsrc sine-periods-per-tick=1 freq=1000 wave=ticks volume=0.8 ! "audio/x-raw, format=S32LE, channels=1, rate=48000" ! alsasink



How the measurement works
-------------------------

One of the channel is designated the _reference channel_. This is the channel
that others will be compared to. Ideally, the channel with the sender's local
audio output should be the picked as the reference channel.

There also the notion of a _window_. The window is where the driftmeasure element
will look for peaks in non-reference channels. This window is centered around
a peak in the reference channel. The driftmeasure element will look for peaks
in the other, non-reference channels, and calculate their position relative to
the peak in the reference channel. If all devices are perfectly in sync, then
so will the peaks be. But if they drifted apart, then the peaks will also be
at a distance to each other (that distance is the drift).

The driftmeasure element operates in one of two different modes. Initially, it
is in the _search mode_. In this mode, it searches for a peak in the reference
channel. If it finds one, two cases may happen:

1: There is not enough audio data _before_ the peak. There must be at least
half the window's size worth of data recorded before the peak. Anything less
than that, and we cannot reliably analyze anything, because there is not
enough data to fill the window. In this case, the recorded data is discarded
and the driftmeausre element continues in the search mode.

2: There is enough audio data before the peak. We can now switch to
_analysis mode._

In analysis mode, we have a peak in the reference channel and enough data
_before_ that peak. Now we need enough data _after_ the peak to be able to
actually perform the analysis. So, we gather data until we have enough.

Once there is enough data, we can perform the analysis. As mentioned above
already, we look for peaks in the non-reference channels within the boundaries
of the window. (This is why we insisted on "gathering enough data" before;
otherwise, we'd have no data we could analyze.) So, if for example the
window size is 1000 ms, and the peak in the reference channel was detected
at position 13000ms in the audio data, then the window will reach from
12500ms to 13500ms (the reference channel peak being in the center). Any
peaks in the non-reference channel we find in this temporal range will be
part of this analysis.

Once we found peaks in non-reference channels, we calculate their temporal
distance from the reference channel peak. To continue the above example,
suppose there are 2 non-reference channels, and there is one peak in the
first non-reference channel, at position 12940ms, and another in the second
non-reference channel, at position 13005ms. Then we calculate their distance
relative to that of the reference channel peak, which occurred at position
13000ms. So, we calculate two drift values:

    12940ms - 13000ms = -60ms (the drift in non-reference channel 1)
    13005ms - 13000ms =   5ms (the drift in non-reference channel 2)

Once we processed all channels in the window, we output the measured drifts
as CSV, discard the data we just analyzed (since we do not need it anymore),
and switch back to search mode, to look for more peaks.


CSV layout
----------

The CSV output looks like this:

    <timestamp>,<drift between reference and non-reference 1>,<drift between reference and non-reference 2>...

The timestamp and drift values are all given in nanoseconds. There is one
column with the timestamp of the measurement (this is where the peak in the
reference channel was found), and one column for each non-reference channel,
in order of the input channels. So, for example, if the sender is attached
to channel #2, and two receivers are at channels #1 and #3, then the first
column would contain the timestamps of the peaks in the reference channel #2,
followed by the related peak in non-reference channel #1, followed by the
related peak in non-reference channel #3.

The output of the example described in the previous section would look like:

    13000000000,-60000000,5000000

Having timestamps is useful in cases the peaks happened at irregular intervals,
for example due to disturbances in the input signal.


Creating a graph out of the CSV data
------------------------------------

For convenience, a GNU R script is provided for creating a graph (as a PNG)
out of the CSV data. The script produces one plot for each non-reference channel,
showing its drift over time. It can also produce filtered trend curves on top
of these graphs to help show the overall trend better.

In addition, the script can trim the CSV data it read to get rid of outliers.
This is useful if errors occurred during measurement.

To run the script, the following R packages must be installed:

* [signal](https://cran.r-project.org/web/packages/signal/)
* [argparser](https://cran.r-project.org/web/packages/argparser/)

In case an error like `could not find function "butter"` appears, check that the
`signal` package is at least version 0.7-7. To install the packages, use the
`install.packages` function in the R shell (with the package name quoted), like this:

    install.packages("argparser")

Example call for producing a graph out of CSV data, with additional filtered
trend graphs enabled and outliers trimmed:

    ./create-graph.r -i csv-data.csv -o plot.png --with-filtered-plot --trim-outliers

Example output (using simulated data):

![example graph output](example-graph-output.png)


Using the Python frontend script
--------------------------------

The Python script conveniently takes care of setting up a GStreamer pipeline
that captures PCM data via PulseAudio and measures drift.

Note that this is a Python 3 script, not a Python 2 one.

To use it, first make sure that GStreamer Python support is present, and that
the `pulsectl` Python module is installed. In Ubuntu, run these commands in
a shell:

    sudo apt install gir1.2-gstreamer-1.0 python3-pip
    pip3 install pulsectl

Then, build the gstdriftmeasure plugin as described above in the
"Building and installing" section. Now you can run the script.

The script requires a PulseAudio source to be explicitely specified. For
convenience, it can list the names and descriptions of the available sources.
Run:

    ./driftmeasure-frontend.py --list-available-sources

Example output:

2 PulseAudio source(s) available

    #0:
             name: "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor"
      description: "Monitor of Built-in Audio Analog Stereo"
    #1:
             name: "alsa_input.pci-0000_00_1f.3.analog-stereo"
      description: "Built-in Audio Analog Stereo"

Note that the "Monitor" sources are not true capture devices. They capture
the output that goes to a sink. In other words, they are _loopback_ devices.
Typically, these are of no interest for drift measurement.

In this example, "alsa_input.pci-0000_00_1f.3.analog-stereo" would be a name
that could be passed to the script, like this:

    ./driftmeasure-frontend.py --source-name="alsa_input.pci-0000_00_1f.3.analog-stereo" --output-csv-filename=output.csv

This would capture PCM data from the analog input device, detect drift there,
and output it to "output.csv".

Normally, the analyzed PCM data is not kept around. This is useful, since it
makes it possible to analyze the drift for a very long time without worrying
about storage space for huge amounts of PCM data. However, sometimes it may
be desirable to also dump the PCM data to WAV. The script can be instructed
to do so. Example:

    ./driftmeasure-frontend.py --source-name="alsa_input.pci-0000_00_1f.3.analog-stereo" --output-csv-filename=output.csv --output-wav-filename=output.wav

Please note that running this for a long time will produce very large WAV
files, so be sure that there is enough storage space available.

By default, the script records data with a sample rate of 96 kHz. This can
be changed with the `--sample-rate` switch. Also, by default, it records
2 channels. As explained above, 1 channel is the reference channel (0 by
default), the others are additional devices. Typically, a multiroom sender
is set as the reference channel, and the receivers are the other channels.
If there is only 1 sender and 1 receiver, then the default of 2 channels
is fine. But if for example there are 3 receivers, it is necessary to
specify that 4 channels must be captured. This is done with the
`--num-channels` switch.

Additional switches for more configuration are listed by running:

    ./driftmeasure-frontend.py --help
