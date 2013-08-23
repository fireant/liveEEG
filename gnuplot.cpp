#include "gnuplot.h"
#include <sstream>
#include <iostream>

GnuPlot::GnuPlot(std::vector<std::string>& legends) {
  std::ostringstream cmd;
  cmd<<"feedgnuplot --lines --nodomain --ymin -0.5 --ymax 4.5";
  for (size_t i=0; i<legends.size(); i++)
    cmd<<" --legend "<<i<<" \""<<legends[i]<<"\"";
  cmd<<" --stream -xlen 400 --geometry 940x450-0+0";
  gfeed_ = popen(cmd.str().c_str(), "w");
}

void GnuPlot::Plot(std::vector<float>& values) {
  for (size_t i=0; i<values.size(); i++)
    fprintf(gfeed_, "%f ", values[i]);
  fprintf(gfeed_, "\nreplot\n");
  fflush(gfeed_);
}
