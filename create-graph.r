#!/usr/bin/env Rscript


# From https://stackoverflow.com/a/50033266/560774
# This suppresses library loading messages
import_library = function(lib_name)
{
	suppressWarnings(suppressMessages(require(lib_name, character.only = TRUE)))
}


import_library('signal');
import_library('argparser');


colors <- matrix(c('red', 'green', 'blue', 'darkred', 'darkgreen', 'darkblue'), 3, 2);
microsecond <- 1e3;
second <- 1e9;


argp <- arg_parser(description = "Common arguments");
argp <- add_argument(argp, "--input", type = "character", help = "input CSV filename")
argp <- add_argument(argp, "--output", type = "character", help = "output PNG image filename")
argp <- add_argument(argp, "--labels", type = "character", nargs = "*", help = "label for each plot (used like: -l label1 label2 label3)")
argp <- add_argument(argp, "--with-filtered-plot", help = "also created filtered plot to show overall trend", flag = TRUE)
argp <- add_argument(argp, "--trim-outliers", help = "trim outliers before producing graph", flag = TRUE)

args <- commandArgs(trailingOnly = TRUE)
if (length(args) == 0)
	args <- c('--help')

opt = parse_args(argp, args);

if (is.null(opt$input))
	stop("Missing input CSV file", call.=FALSE);

if (is.null(opt$output))
	stop("Missing output image file", call.=FALSE);

labels <- c();
if (!is.na(opt$labels))
	labels <- unlist(strsplit(opt$labels, "[,]"));


cat(sprintf('Producing graph out of file "%s" and writing it to "%s"\n', opt$input, opt$output))


csv <- read.csv(opt$input, header=FALSE, sep=",");

# drop last column if only consists of NaN (can happen if all rows have a trailing comma)
if (all(is.na(csv[,length(csv)])))
	csv <- csv[,1:(length(csv)-1)];

# drop all incomplete rows to avoid NaN values
for (i in 1:length(csv))
	csv[,i] <- as.numeric(as.character(csv[,i]));
csv <- na.omit(csv);

# trim outliers if requested
if (opt$trim_outliers)
{
	for (i in 1:length(csv))
		csv <- csv[!csv[,i] %in% boxplot.stats(csv[,i])$out,]
}

# -1 to exclude the timestamps
num_channels <- (length(csv)-1);
if (num_channels <= 0)
	stop("Not enough channels in CSV", call.=FALSE);

# fill any missing labels
if (length(labels) < num_channels)
{
	for (i in length(labels):(num_channels-1))
	{
		label <- sprintf('channel %d', i)
		labels <- c(labels, label)
	}
}

# find reasonable ylim values
# add 20% of the min-max distance above max and below min to make some room in the visual presentation
min_y <- min(csv[,2:(num_channels+1)]) / microsecond;
max_y <- max(csv[,2:(num_channels+1)]) / microsecond;
distance <- (max_y - min_y);
min_y <- min_y - distance * 0.2;
max_y <- max_y + distance * 0.2;

# width/height are in inches, because then, "res" equals a DPI value
png(filename = opt$output, width = 12, height = 7, units = 'in', res = 300);
# TODO: svg would be better ... but does not contain text

# create lowpass filter for yvalues
sample_rate <- 512
lowpass_filter <- butter(2, 1/sample_rate, type = "low");

# set up plot: axes, grid, horizontal line at y=0, at y=100us, y=-100us

xlim <- c(csv[1,1], csv[length(csv[,1]), 1]) / second
ylim <- c(min_y, max_y)
plot(NULL, NULL, type = 'l', xlim = xlim, ylim = ylim, xlab = 'playtime (seconds)', ylab = 'drift (microseconds)');
grid(lwd = 1);
abline(0, 0);

# convert timestamps to seconds for plot
xvalues <- csv[,1] / second;

# initialize legend descriptions
descriptions <- labels;

# amount of padding we add below
padding <- sample_rate * 2

for (i in 0:(num_channels-1))
{
	yvalues_unfiltered <- csv[,(i+2)] / microsecond;

	colidx <- (i %% num_channels) + 1;
	faintcolor <- colors[colidx, 2];

	# plot 25%/50%/75% quantiles as faint horizontal lines (0% and 100% are min/max, just use them for log output)
	q <- quantile(yvalues_unfiltered, probs = c(0.0, 0.25, 0.5, 0.75, 1.0));
	abline(q[2], 0, lwd = 0.5, lty = 1, col = faintcolor);
	abline(q[3], 0, lwd = 0.5, lty = 1, col = faintcolor);
	abline(q[4], 0, lwd = 0.5, lty = 1, col = faintcolor);
	cat(sprintf("%s quantiles:  0.25: %f  0.5: %f  0.75: %f   min: %f  max: %f\n", labels[i + 1], q[2], q[3], q[4], q[1], q[5]));

	# plot unfiltered yvalues with faint lines
	lines(xvalues, yvalues_unfiltered, col = faintcolor, lwd = 0.3, lty = 1);

	if (opt$with_filtered_plot)
	{
		strongcolor <- colors[colidx, 1];

		# produce a filtered version of the data to see the overall trend better
		# pad the unfiltered values to make sure the start and end of the filtered
		# trend curve does not start/end at zero
		yvalues_unfiltered_padded <- c(rep(yvalues_unfiltered[1], padding), yvalues_unfiltered, rep(yvalues_unfiltered[length(yvalues_unfiltered)], padding))
		yvalues_filtered <- filtfilt(lowpass_filter, yvalues_unfiltered_padded);
		yvalues_filtered <- yvalues_filtered[(padding + 1):(length(yvalues_filtered) - padding)]

		# plot filtered yvalues with strong lines
		lines(xvalues, yvalues_filtered, col = strongcolor, lwd = 2.0, lty = 1);
	}
}

legend(x = 'topleft', descriptions, lty = 1, lwd = 2.5, col = colors[,1]);


# invisible() is from https://stackoverflow.com/a/15406186/560774
# This suppresses the "null device" message
invisible(dev.off());
