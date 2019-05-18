#ifndef GSTDRIFTMEASURE_H
#define GSTDRIFTMEASURE_H

#include <gst/gst.h>


G_BEGIN_DECLS


#define GST_TYPE_DRIFT_MEASURE             (gst_drift_measure_get_type())
#define GST_DRIFT_MEASURE(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_DRIFT_MEASURE,GstDriftMeasure))
#define GST_DRIFT_MEASURE_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_DRIFT_MEASURE,GstDriftMeasureClass))
#define GST_DRIFT_MEASURE_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_DRIFT_MEASURE, GstDriftMeasureClass))
#define GST_DRIFT_MEASURE_CAST(obj)        ((GstDriftMeasure *)(obj))
#define GST_IS_DRIFT_MEASURE(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_DRIFT_MEASURE))
#define GST_IS_DRIFT_MEASURE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_DRIFT_MEASURE))


typedef struct _GstDriftMeasure GstDriftMeasure;
typedef struct _GstDriftMeasureClass GstDriftMeasureClass;


GType gst_drift_measure_get_type(void);


G_END_DECLS


#endif /* GSTDRIFTMEASURE_H */
