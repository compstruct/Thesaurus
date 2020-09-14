#include <cmath>
#include "stats.h"

RunningStats::RunningStats(g_string& name) throw () :
    minimum(INFINITY), maximum(-INFINITY), mean(0), varNumer(0), weightSum(0), numSamples(0), name(name) {
}

void RunningStats::reset() throw () {
    minimum = INFINITY;
    maximum = -INFINITY;
    mean = 0;
    varNumer = 0;
    weightSum = 0;
}

void RunningStats::add(double sample, double weight) throw () {
    ++numSamples;
    if (!std::isnan(sample) && !std::isnan(weight)) { // NaN -> invalid sample
        if (sample < minimum) minimum = sample;
        if (sample > maximum) maximum = sample;
        if (weight > 0) {
            double s = weightSum + weight;
            double q = sample - mean;
            double r = q * weight / s;
            mean += r;
            varNumer += r * weightSum * q;
            weightSum = s;
        }
    }
}

double RunningStats::getMin() const throw () {
    return minimum;
}

double RunningStats::getMax() const throw () {
    return maximum;
}

double RunningStats::getMean() const throw () {
    return mean;
}

double RunningStats::getStdDev() const throw () {
    return weightSum == 0 ? 0 : sqrt(varNumer / weightSum);
}

void RunningStats::combineWith(const RunningStats &rhs) throw () {
    minimum = fmin(minimum, rhs.minimum);
    maximum = fmax(maximum, rhs.maximum);
    if (weightSum == 0) {
        mean = rhs.mean;
        varNumer = rhs.varNumer;
        weightSum = rhs.weightSum;
    } else if (rhs.weightSum != 0) {
        mean = ((mean * weightSum + rhs.mean * rhs.weightSum) / (weightSum + rhs.weightSum));
        varNumer += rhs.varNumer;
        weightSum += rhs.weightSum;
    }
}

void RunningStats::dump() {
    info("%s: Min = %f, Mean = %f, Max = %f, StdDev = %f", this->name.c_str(), this->getMin(), this->getMean(), this->getMax(), this->getStdDev());
}

void RunningStats::dumpFile(std::ofstream* file) {
    (*file) << this->name.c_str() << ": Min = " << this->getMin() << ", Mean = " << this->getMean() << ", Max = " << this->getMax() << "\n";
}
