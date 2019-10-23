/*
Copyright 2017-2019 University of Huddersfield.
Licensed under the BSD-3 License.
See LICENSE file in the project root for full license information.
This project has received funding from the European Research Council (ERC)
under the European Union’s Horizon 2020 research and innovation programme
(grant agreement No 725899).
*/

#pragma once

#include "../util/AlgorithmUtils.hpp"
#include "../util/FluidEigenMappings.hpp"
#include "../util/MedianFilter.hpp"
#include "../../data/TensorTypes.hpp"
#include <Eigen/Core>

namespace fluid {
namespace algorithm {

class HPSS
{
public:
  using ArrayXXd = Eigen::ArrayXXd;
  using ArrayXXcd = Eigen::ArrayXXcd;
  using ArrayXcd = Eigen::ArrayXcd;

  enum HPSSMode { kClassic, kCoupled, kAdvanced };

  void init(int nBins, int maxVSize, int maxHSize, int vSize, int hSize,
            int mode, double hThresholdX1, double hThresholdY1,
            double hThresholdX2, double hThresholdY2, double pThresholdX1,
            double pThresholdY1, double pThresholdX2, double pThresholdY2)
  {
    assert(maxVSize % 2);
    assert(maxHSize % 2);
    assert(vSize % 2);
    assert(hSize % 2);
    assert(hSize <= maxHSize);
    assert(vSize <= maxVSize);
    assert(mode >= 0 && mode <= 3);
    mMaxH = ArrayXXd::Zero(nBins, maxHSize);
    mMaxV = ArrayXXd::Zero(nBins, maxHSize);
    mMaxBuf = ArrayXXd::Zero(nBins, maxHSize);

    mBins = nBins;
    mMaxVSize = maxVSize;
    mMaxHSize = maxHSize;
    mVSize = vSize;
    mMode = mode;
    setHSize(hSize);

    mHThresholdX1 = hThresholdX1;
    mHThresholdX2 = hThresholdX2;
    mHThresholdY1 = hThresholdY1;
    mHThresholdY2 = hThresholdY2;

    mPThresholdX1 = pThresholdX1;
    mPThresholdX2 = pThresholdX2;
    mPThresholdY1 = pThresholdY1;
    mPThresholdY2 = pThresholdY2;

    mInitialized = true;
  }


  void processFrame(const ComplexVectorView in, ComplexMatrixView out)
  {
    using namespace Eigen;

    int h2 = (mHSize - 1) / 2;
    int v2 = (mVSize - 1) / 2;

    ArrayXcd frame = _impl::asEigen<Array>(in);
    ArrayXd  mag = frame.abs().real();
    mV.block(0, 0, mBins, mHSize - 1) = mV.block(0, 1, mBins, mHSize - 1);
    mBuf.block(0, 0, mBins, mHSize - 1) = mBuf.block(0, 1, mBins, mHSize - 1);
    ArrayXd padded =
        ArrayXd::Zero(mVSize + mVSize * std::ceil(mBins / double(mVSize)));
    ArrayXd resultV = ArrayXd::Zero(padded.size());
    padded.segment(v2, mBins) = mag;
    MedianFilter mVMedianFilter = MedianFilter(padded, mVSize);
    mVMedianFilter.process(resultV);
    mV.block(0, mHSize - 1, mBins, 1) = resultV.segment(v2, mBins);
    mBuf.block(0, mHSize - 1, mBins, 1) = frame;
    ArrayXd tmpRow = ArrayXd::Zero(2 * mHSize);
    for (int i = 0; i < mBins; i++)
    {
      mHFilters[i].insertRight(mag(i));
      mHFilters[i].process(tmpRow);
      mH.row(i) = tmpRow.segment(h2, mHSize).transpose();
    }
    ArrayXXcd result(mBins, 3);
    ArrayXd   harmonicMask = ArrayXd::Ones(mBins);
    ArrayXd   percussiveMask = ArrayXd::Ones(mBins);
    ArrayXd   residualMask =
        mMode == kAdvanced ? ArrayXd::Ones(mBins) : ArrayXd::Zero(mBins);

    switch (mMode)
    {
    case kClassic:
    {
      ArrayXd HV = mH.col(0) + mV.col(0);
      ArrayXd mult = (1.0 / HV.max(epsilon));
      harmonicMask = (mH.col(0) * mult);
      percussiveMask = (mV.col(0) * mult);
      break;
    }
    case kCoupled:
    {
      harmonicMask = ((mH.col(0) / mV.col(0)) >
                      makeThreshold(mHThresholdX1, mHThresholdY1, mHThresholdX2,
                                    mHThresholdY2))
                         .cast<double>();
      percussiveMask = 1 - harmonicMask;
      break;
    }
    case kAdvanced:
    {
      harmonicMask = ((mH.col(0) / mV.col(0)) >
                      makeThreshold(mHThresholdX1, mHThresholdY1, mHThresholdX2,
                                    mHThresholdY2))
                         .cast<double>();
      percussiveMask = ((mV.col(0) / mH.col(0)) >
                        makeThreshold(mPThresholdX1, mPThresholdY1,
                                      mPThresholdX2, mPThresholdY2))
                           .cast<double>();
      residualMask = residualMask * (1 - harmonicMask);
      residualMask = residualMask * (1 - percussiveMask);
      ArrayXd maskNorm =
          (1. / (harmonicMask + percussiveMask + residualMask)).max(epsilon);
      harmonicMask = harmonicMask * maskNorm;
      percussiveMask = percussiveMask * maskNorm;
      residualMask = residualMask * maskNorm;
      break;
    }
    }

    result.col(0) = mBuf.col(0) * harmonicMask.min(1.0);
    result.col(1) = mBuf.col(0) * percussiveMask.min(1.0);
    result.col(2) = mBuf.col(0) * residualMask.min(1.0);
    out = _impl::asFluid(result);
  }

  void setMode(int mode)
  {
    assert(mode >= 0 && mode <= 2);
    mMode = mode;
  }

  void setHSize(int newHSize)
  {
    using namespace Eigen;
    assert(newHSize <= mMaxHSize);
    assert(newHSize % 2);
    mH = mMaxH.block(0, 0, mBins, newHSize);
    mV = mMaxV.block(0, 0, mBins, newHSize);
    mBuf = mMaxBuf.block(0, 0, mBins, newHSize);
    mH.setZero();
    mV.setZero();
    mBuf.setZero();
    std::vector<MedianFilter> newFilters;
    mHFilters.swap(newFilters);
    for (int i = 0; i < mBins; i++)
    {
      ArrayXd tmp = ArrayXd::Zero(2 * newHSize);
      mHFilters.emplace_back(MedianFilter(tmp, newHSize));
    }
    mHSize = newHSize;
  }

  void setVSize(int newVSize)
  {
    assert(newVSize <= mMaxVSize);
    assert(newVSize % 2);
    mVSize = newVSize;
  }

  void setHThresholdX1(double x)
  {
    assert(0 <= x && x <= 1);
    mHThresholdX1 = x;
  }

  void setHThresholdX2(double x)
  {
    assert(0 <= x && x <= 1);
    mHThresholdX2 = x;
  }

  void setPThresholdX1(double x)
  {
    assert(0 <= x && x <= 1);
    mPThresholdX1 = x;
  }

  void setPThresholdX2(double x)
  {
    assert(0 <= x && x <= 1);
    mPThresholdX2 = x;
  }

  void setHThresholdY1(double y) { mHThresholdY1 = y; }

  void setHThresholdY2(double y) { mHThresholdY2 = y; }

  void setPThresholdY1(double y) { mPThresholdY1 = y; }

  void setPThresholdY2(double y) { mPThresholdY2 = y; }


private:
  Eigen::ArrayXd makeThreshold(double x1, double y1, double x2, double y2)
  {
    using namespace Eigen;
    ArrayXd threshold = ArrayXd::Ones(mBins);
    int     kneeStart = floor(x1 * mBins);
    int     kneeEnd = floor(x2 * mBins);
    int     kneeLength = kneeEnd - kneeStart;
    threshold.segment(0, kneeStart) =
        ArrayXd::Constant(kneeStart, 10).pow(y1 / 20.0);
    threshold.segment(kneeStart, kneeLength) =
        ArrayXd::Constant(kneeLength, 10)
            .pow(ArrayXd::LinSpaced(kneeLength, y1, y2) / 20.0);
    threshold.segment(kneeEnd, mBins - kneeEnd) =
        ArrayXd::Constant(mBins - kneeEnd, 10).pow(y2 / 20.0);
    return threshold;
  }

  std::vector<MedianFilter> mHFilters;

  size_t    mBins{513};
  size_t    mMaxVSize{101};
  size_t    mMaxHSize{101};
  size_t    mVSize{31};
  size_t    mHSize{17};
  int       mMode{0};
  ArrayXXd  mMaxH;
  ArrayXXd  mMaxV;
  ArrayXXcd mMaxBuf;
  ArrayXXd  mV;
  ArrayXXd  mH;
  ArrayXXcd mBuf;
  double    mHThresholdX1{0.0};
  double    mHThresholdY1{0.0};
  double    mHThresholdX2{0.0};
  double    mHThresholdY2{0.0};
  double    mPThresholdX1{0.0};
  double    mPThresholdY1{0.0};
  double    mPThresholdX2{0.0};
  double    mPThresholdY2{0.0};
  bool      mInitialized = false;
};
} // namespace algorithm
} // namespace fluid
