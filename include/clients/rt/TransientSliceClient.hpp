/*
Copyright 2017-2019 University of Huddersfield.
Licensed under the BSD-3 License.
See LICENSE file in the project root for full license information.
This project has received funding from the European Research Council (ERC)
under the European Union’s Horizon 2020 research and innovation programme
(grant agreement No 725899).
*/
#pragma once

#include "../common/AudioClient.hpp"
#include "../common/BufferedProcess.hpp"
#include "../common/FluidBaseClient.hpp"
#include "../common/FluidNRTClientWrapper.hpp"
#include "../common/ParameterConstraints.hpp"
#include "../common/ParameterSet.hpp"
#include "../common/ParameterTrackChanges.hpp"
#include "../common/ParameterTypes.hpp"
#include "../../algorithms/public/TransientSegmentation.hpp"
#include <tuple>

namespace fluid {
namespace client {

enum TransientParamIndex {
  kOrder,
  kBlockSize,
  kPadding,
  kSkew,
  kThreshFwd,
  kThreshBack,
  kWinSize,
  kDebounce,
  kMinSeg
};

auto constexpr TransientParams = defineParameters(
    LongParam("order", "Order", 20, Min(10), LowerLimit<kWinSize>(),
              UpperLimit<kBlockSize>()),
    LongParam("blockSize", "Block Size", 256, Min(100), LowerLimit<kOrder>()),
    LongParam("padSize", "Padding", 128, Min(0)),
    FloatParam("skew", "Skew", 0, Min(-10), Max(10)),
    FloatParam("threshFwd", "Forward Threshold", 2, Min(0)),
    FloatParam("threshBack", "Backward Threshold", 1.1, Min(0)),
    LongParam("windowSize", "Window Size", 14, Min(0), UpperLimit<kOrder>()),
    LongParam("clumpLength", "Clumping Window Length", 25, Min(0)),
    LongParam("minSliceLength", "Minimum Length of Slice", 1000));


template <typename T>
class TransientsSliceClient
    : public FluidBaseClient<decltype(TransientParams), TransientParams>,
      public AudioIn,
      public AudioOut
{
  using HostVector = FluidTensorView<T, 1>;

public:
  TransientsSliceClient(ParamSetViewType& p) : FluidBaseClient(p)
  {
    FluidBaseClient::audioChannelsIn(1);
    FluidBaseClient::audioChannelsOut(1);
  }

  void process(std::vector<HostVector>& input, std::vector<HostVector>& output,
               FluidContext& c, bool reset = false)
  {
    using namespace std;

    if (!input[0].data() || !output[0].data()) return;

    static constexpr unsigned iterations = 3;
    static constexpr bool     refine = false;
    static constexpr double   robustFactor = 3.0;

    size_t order = get<kOrder>();
    size_t blockSize = get<kBlockSize>();
    size_t padding = get<kPadding>();
    size_t hostVecSize = input[0].size();
    size_t maxWinIn = 2 * blockSize + padding;
    size_t maxWinOut = maxWinIn; // blockSize - padding;

    if (mTrackValues.changed(order, blockSize, padding, hostVecSize) ||
        !mExtractor.initialized())
    {
      // mExtractor.reset(new algorithm::TransientSegmentation(order,
      // iterations, robustFactor));
      mExtractor.init(order, iterations, robustFactor, blockSize, padding);
      // mExtractor->prepareStream(blockSize, padding);
      mBufferedProcess.hostSize(hostVecSize);
      mBufferedProcess.maxSize(maxWinIn, maxWinOut,
                               FluidBaseClient::audioChannelsIn(),
                               FluidBaseClient::audioChannelsOut());
    }

    double skew = pow(2, get<kSkew>());
    double threshFwd = get<kThreshFwd>();
    double thresBack = get<kThreshBack>();
    size_t halfWindow = round(get<kWinSize>() / 2);
    size_t debounce = get<kDebounce>();
    size_t minSeg = get<kMinSeg>();

    mExtractor.setDetectionParameters(skew, threshFwd, thresBack, halfWindow,
                                      debounce, minSeg);

    RealMatrix in(1, hostVecSize);

    in.row(0) = input[0]; // need to convert float->double in some hosts
    mBufferedProcess.push(RealMatrixView(in));

    mBufferedProcess.process(mExtractor.inputSize(), mExtractor.hopSize(),
                             mExtractor.hopSize(), c, reset,
                             [this](RealMatrixView in, RealMatrixView out) {
                               mExtractor.process(in.row(0), out.row(0));
                             });

    RealMatrix out(1, hostVecSize);
    mBufferedProcess.pull(RealMatrixView(out));

    if (output[0].data()) output[0] = out.row(0);
  }

  size_t latency()
  {
    return get<kPadding>() + get<kBlockSize>() - get<kOrder>();
  }

private:
  ParameterTrackChanges<size_t, size_t, size_t, size_t> mTrackValues;
  // std::unique_ptr<algorithm::TransientSegmentation> mExtractor;
  algorithm::TransientSegmentation mExtractor{get<kOrder>(), 3, 3.0};

  BufferedProcess   mBufferedProcess;
  FluidTensor<T, 1> mTransients;
  size_t            mHostSize{0};
  size_t            mOrder{0};
  size_t            mBlocksize{0};
  size_t            mPadding{0};
};

auto constexpr NRTTransientSliceParams = makeNRTParams<TransientsSliceClient>(
    {InputBufferParam("source", "Source Buffer")},
    {BufferParam("indices", "Indices Buffer")});

template <typename T>
using NRTTransientSliceClient =
    NRTSliceAdaptor<TransientsSliceClient<T>, decltype(NRTTransientSliceParams),
                    NRTTransientSliceParams, 1, 1>;

template <typename T>
using NRTThreadedTransientSliceClient =
    NRTThreadingAdaptor<NRTTransientSliceClient<T>>;

} // namespace client
} // namespace fluid