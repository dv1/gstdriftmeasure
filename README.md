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


Test signal
-----------

The driftmeasure element looks for peaks. The test signal must be designed
in a way that ensure that there is exactly one peak followed by a period of
silence. This must be a mono signal.

One recommended signal structure: one sine wave period of 1000 Hz (we call
this the _signal pulse_), followed by one second of silence. Then, another
signal pulse (exactly like the previous one) appears, again followed by one
second of silence etc. This allows for detecting a drift of up to 500 ms.


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

The CSV output is like this:

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

Example call for producing a graph out of CSV data, with additional filtered
trend graphs enabled and outliers trimmed:

    ./create-graph.r -i csv-data.csv -o plot.png --with-filtered-plot --trim-outliers
