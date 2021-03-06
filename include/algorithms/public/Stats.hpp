/*
Part of the Fluid Corpus Manipulation Project (http://www.flucoma.org/)
Copyright 2017-2019 University of Huddersfield.
Licensed under the BSD-3 License.
See license.md file in the project root for full license information.
This project has received funding from the European Research Council (ERC)
under the European Union’s Horizon 2020 research and innovation programme
(grant agreement No 725899).
*/
#pragma once

#include "../util/FluidEigenMappings.hpp"
#include "../../data/TensorTypes.hpp"
#include "../../data/FluidIndex.hpp"
#include <Eigen/Core>
#include <cassert>
#include <cmath>

namespace fluid {
namespace algorithm {

class Stats
{
public:
  void init(index numDerivatives, double low, double mid, double high)
  {
    assert(numDerivatives <= 2);
    mNumDerivatives = numDerivatives;
    mLow = low / 100.0;
    mMiddle = mid / 100.0;
    mHigh = high / 100.0;
  }
  index numStats() { return 7; }

  Eigen::ArrayXd computeStats(Eigen::Ref<Eigen::ArrayXd> input)
  {
    using namespace Eigen;
    using namespace std;
    index   length = input.size();
    ArrayXd out = ArrayXd::Zero(7);
    double  mean = input.mean();
    double  stdev = sqrt((input - mean).square().mean());
    double  skewness = ((input - mean) / (stdev == 0 ? 1 : stdev)).cube().mean();
    double  kurtosis = ((input - mean) / (stdev == 0 ? 1 : stdev)).pow(4).mean();
    ArrayXd sorted = input;
    sort(sorted.data(), sorted.data() + length);
    double low = sorted(lrint(mLow * (length - 1)));
    double mid = sorted(lrint(mMiddle * (length - 1)));
    double high = sorted(lrint(mHigh * (length - 1)));
    out << mean, stdev, skewness, kurtosis, low, mid, high;
    return out;
  }

  void process(const RealVectorView in, RealVectorView out)
  {
    using namespace Eigen;
    using namespace _impl;
    using fluid::Slice;
    assert(out.size() == numStats() * (mNumDerivatives + 1));
    ArrayXd input = asEigen<Array>(in);
    index   length = input.size();
    ArrayXd raw = computeStats(input);
    out(Slice(0, numStats())) = asFluid(raw);
    if (mNumDerivatives > 0)
    {
      ArrayXd diff1 =
          input.segment(1, length - 1) - input.segment(0, length - 1);
      ArrayXd d1 = computeStats(diff1);
      out(Slice(numStats(), numStats())) = asFluid(d1);
      if (mNumDerivatives > 1)
      {
        ArrayXd diff2 =
            diff1.segment(1, length - 2) - diff1.segment(0, length - 2);
        ArrayXd d2 = computeStats(diff2);
        out(Slice(2 * numStats(), numStats())) = asFluid(d2);
      }
    }
  }

  index  mNumDerivatives{0};
  double mLow{0};
  double mMiddle{0.5};
  double mHigh{1};
};
} // namespace algorithm
} // namespace fluid
