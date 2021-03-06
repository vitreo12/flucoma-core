#pragma once

#include "../common/BufferAdaptor.hpp"
#include "../common/FluidBaseClient.hpp"
#include "../common/MemoryBufferAdaptor.hpp"
#include "../common/OfflineClient.hpp"
#include "../common/ParameterSet.hpp"
#include "../common/ParameterTypes.hpp"
#include "../common/SpikesToTimes.hpp"
#include "../../data/FluidIndex.hpp"
#include "../../data/FluidTensor.hpp"
#include "../../data/TensorTypes.hpp"
#include <deque>
#include <future>
#include <thread>
#include <vector>

namespace fluid {
namespace client {

namespace impl {
//////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename B>
auto constexpr makeWrapperInputs(B b)
{
  return defineParameters(std::forward<B>(b),
                          LongParam("startFrame", "Source Offset", 0, Min(0)),
                          LongParam("numFrames", "Number of Frames", -1),
                          LongParam("startChan", "Start Channel", 0, Min(0)),
                          LongParam("numChans", "Number of Channels", -1));
}

template <typename... B>
auto constexpr makeWrapperOutputs(B... b)
{
  return defineParameters(std::forward<B>(b)...);
}

template <typename T, size_t N, size_t... Is>
auto constexpr spitIns(T (&a)[N], std::index_sequence<Is...>)
{
  return makeWrapperInputs(std::forward<T>(a[Is])...);
}

template <typename T, size_t N, size_t... Is>
auto constexpr spitOuts(T (&a)[N], std::index_sequence<Is...>)
{
  return makeWrapperOutputs(std::forward<T>(a[Is])...);
}
//////////////////////////////////////////////////////////////////////////////////////////////////////
using InputBufferSpec = ParamSpec<InputBufferT, Fixed<false>>;
using BufferSpec = ParamSpec<BufferT, Fixed<false>>;
//////////////////////////////////////////////////////////////////////////////////////////////////////

template <size_t... Is, size_t... Js, typename... Ts, typename... Us>
constexpr auto joinParameterDescriptors(
    ParameterDescriptorSet<std::index_sequence<Is...>, std::tuple<Ts...>> x,
    ParameterDescriptorSet<std::index_sequence<Js...>, std::tuple<Us...>> y)
{
  return ParameterDescriptorSet<
      typename JoinOffsetSequence<std::index_sequence<Is...>,
                                  std::index_sequence<Js...>>::type,
      std::tuple<Ts..., Us...>>{
      std::tuple_cat(x.descriptors(), y.descriptors())};
}

//////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename HostMatrix, typename HostVectorView>
struct StreamingControl;

template <template <typename, typename> class AdaptorType, class RTClient,
          typename ParamType, ParamType& PD, size_t Ins, size_t Outs>
class NRTClientWrapper : public OfflineIn, public OfflineOut
{
public:
  // Host buffers are always float32 (?)
  using HostVector = FluidTensor<float, 1>;
  using HostMatrix = FluidTensor<float, 2>;
  using HostVectorView = FluidTensorView<float, 1>;
  using HostMatrixView = FluidTensorView<float, 2>;
  static constexpr auto isControl =
      std::is_same<AdaptorType<HostMatrix, HostVectorView>,
                   StreamingControl<HostMatrix, HostVectorView>>();
  static constexpr auto decideOuts = isControl ? 1 : Outs;

  using ParamDescType = ParamType;
  using ParamSetType = ParameterSet<ParamDescType>;
  using ParamSetViewType = ParameterSetView<ParamDescType>;
  using RTParamDescType = typename RTClient::ParamDescType;
  using RTParamSetViewType = ParameterSetView<typename RTClient::ParamDescType>;

  constexpr static ParamDescType& getParameterDescriptors() { return PD; }

  // The client will be accessing its parameter by a bunch of indices that need
  // ofsetting now
  //  using Client =
  //  RTClient<impl::ParameterSet_Offset<Params,ParamOffset>,T,U>;
  // None of that for outputs though

  static constexpr size_t ParamOffset = (Ins * 5) + decideOuts;
  using WrappedClient = RTClient; //<ParameterSet_Offset<Params,ParamOffset>,T>;

  NRTClientWrapper(ParamSetViewType& p)
      : mParams{p}, mRealTimeParams{RTClient::getParameterDescriptors(),
                                    p.template subset<ParamOffset>()},
        mClient{mRealTimeParams}
  {}

  template <std::size_t N>
  auto& get() noexcept
  {
    return mParams.get().template get<N>();
  }
  //  template <std::size_t N> bool changed() noexcept { return mParams.template
  //  changed<N>(); }

  index audioChannelsIn() const noexcept { return 0; }
  index audioChannelsOut() const noexcept { return 0; }
  index controlChannelsIn() const noexcept { return 0; }
  index controlChannelsOut() const noexcept { return 0; }
  /// Map delegate audio / control channels to audio buffers
  index audioBuffersIn() const noexcept { return mClient.audioChannelsIn(); }
  index audioBuffersOut() const noexcept
  {
    return isControl ? 1 : mClient.audioChannelsOut();
  }

  void setParams(ParamSetViewType& p)
  {
    mParams = p;
    mRealTimeParams = RTParamSetViewType(RTClient::getParameterDescriptors(),
                                         p.template subset<ParamOffset>());
    mClient.setParams(mRealTimeParams);
  }

  Result process(FluidContext& c)
  {


    auto constexpr inputCounter = std::make_index_sequence<Ins>();
    auto constexpr outputCounter = std::make_index_sequence<decideOuts>();

    auto inputBuffers = fetchInputBuffers(inputCounter);
    auto outputBuffers = fetchOutputBuffers(outputCounter);

    std::array<index, Ins> inFrames;
    std::array<index, Ins> inChans;

    // check buffers exist
    index count = 0;
    for (auto&& b : inputBuffers)
    {

      index requestedFrames = b.nFrames;
      index requestedChans = b.nChans;

      auto rangeCheck = bufferRangeCheck(
          b.buffer, b.startFrame, requestedFrames, b.startChan, requestedChans);

      if (!rangeCheck.ok()) return rangeCheck;

      inFrames[asUnsigned(count)] = requestedFrames;
      inChans[asUnsigned(count)] = requestedChans;
      mClient.sampleRate(BufferAdaptor::ReadAccess(b.buffer).sampleRate());
      count++;
    }

    if (std::all_of(outputBuffers.begin(), outputBuffers.end(), [](auto& b) {
          if (!b) return true;

          BufferAdaptor::Access buf(b);
          return !buf.exists();
        }))
      return {Result::Status::kError, "No valid output has been set"}; // error


    Result r{Result::Status::kOk, ""};

    // Remove non-existent output buffers from the output buffers vector, so
    // clients don't try and use them
    std::transform(
        outputBuffers.begin(), outputBuffers.end(), outputBuffers.begin(),
        [&r](auto& b) -> BufferAdaptor* {
          if (!b) return nullptr;
          BufferAdaptor::Access buf(b);
          if (!buf.exists())
          {
            r.set(Result::Status::kWarning);
            r.addMessage("One or more of your output buffers doesn't exist\n");
          }
          return buf.exists() ? b : nullptr;
        });


    index numFrames = *std::min_element(inFrames.begin(), inFrames.end());
    index numChannels = *std::min_element(inChans.begin(), inChans.end());

    Result processResult = AdaptorType<HostMatrix, HostVectorView>::process(
        mClient, inputBuffers, outputBuffers, numFrames, numChannels, c);

    if (!processResult.ok())
    {
      r.set(processResult.status());
      r.addMessage(processResult.message());
    }

    return r;
  }

private:
  template <size_t I>
  BufferProcessSpec fetchInputBuffer()
  {
    return {get<I>().get(), get<I + 1>(), get<I + 2>(), get<I + 3>(),
            get<I + 4>()};
  }

  template <size_t... Is>
  std::array<BufferProcessSpec, sizeof...(Is)>
      fetchInputBuffers(std::index_sequence<Is...>)
  {
    return {{fetchInputBuffer<Is * 5>()...}};
  }

  template <size_t... Is>
  std::array<BufferAdaptor*, sizeof...(Is)>
      fetchOutputBuffers(std::index_sequence<Is...>)
  {
    return {{get<Is + (Ins * 5)>().get()...}};
  }

  std::reference_wrapper<ParamSetViewType> mParams;
  RTParamSetViewType                       mRealTimeParams;
  WrappedClient                            mClient;
};
//////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename HostMatrix, typename HostVectorView>
struct Streaming
{
  template <typename Client, typename InputList, typename OutputList>
  static Result process(Client& client, InputList& inputBuffers,
                        OutputList& outputBuffers, index nFrames, index nChans,
                        FluidContext& c)
  {
    // To account for process latency we need to copy the buffers with padding
    std::vector<HostMatrix> outputData;
    std::vector<HostMatrix> inputData;

    outputData.reserve(outputBuffers.size());
    inputData.reserve(inputBuffers.size());

    index padding = client.latency();

    std::fill_n(std::back_inserter(outputData), outputBuffers.size(),
                HostMatrix(nChans, nFrames + padding));
    std::fill_n(std::back_inserter(inputData), inputBuffers.size(),
                HostMatrix(nChans, nFrames + padding));

    double sampleRate{0};

    for (index i = 0; i < nChans; ++i)
    {
      std::vector<HostVectorView> inputs;
      inputs.reserve(inputBuffers.size());
      for (index j = 0; j < asSigned(inputBuffers.size()); ++j)
      {
        BufferAdaptor::ReadAccess thisInput(inputBuffers[asUnsigned(j)].buffer);
        if (i == 0 && j == 0) sampleRate = thisInput.sampleRate();
        inputData[asUnsigned(j)].row(i)(Slice(0, nFrames)) =
            thisInput.samps(inputBuffers[asUnsigned(j)].startFrame, nFrames,
                            inputBuffers[asUnsigned(j)].startChan + i);
        inputs.emplace_back(inputData[asUnsigned(j)].row(i));
      }

      std::vector<HostVectorView> outputs;
      outputs.reserve(outputBuffers.size());
      for (index j = 0; j < asSigned(outputBuffers.size()); ++j)
        outputs.emplace_back(outputData[asUnsigned(j)].row(i));

      if (c.task()) c.task()->iterationUpdate(i, nChans);

      client.reset();
      client.process(inputs, outputs, c);
    }

    for (index i = 0; i < asSigned(outputBuffers.size()); ++i)
    {
      if (!outputBuffers[asUnsigned(i)]) continue;
      BufferAdaptor::Access thisOutput(outputBuffers[asUnsigned(i)]);
      Result                r = thisOutput.resize(nFrames, nChans, sampleRate);
      if (!r.ok()) return r;
      for (index j = 0; j < nChans; ++j)
        thisOutput.samps(j) = outputData[asUnsigned(i)].row(j)(Slice(padding));
    }

    return {};
  }
};
//////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename HostMatrix, typename HostVectorView>
struct StreamingControl
{
  template <typename Client, typename InputList, typename OutputList>
  static Result process(Client& client, InputList& inputBuffers,
                        OutputList& outputBuffers, index nFrames, index nChans,
                        FluidContext& c)
  {
    // To account for process latency we need to copy the buffers with padding
    std::vector<HostMatrix> inputData;
    index                   nFeatures = client.controlChannelsOut();
    //      outputData.reserve(nFeatures);
    inputData.reserve(inputBuffers.size());

    index padding = client.latency();
    index controlRate = client.controlRate();
    index nHops = (nFrames + padding) / controlRate;

    // in contrast to the plain streaming case, we're going to call process()
    // iteratively with a vector size = the control vector size, so we get KR
    // where expected
    // TODO make this whole mess less baroque and opaque

    std::fill_n(std::back_inserter(inputData), inputBuffers.size(),
                HostMatrix(nChans, nFrames + padding));
    HostMatrix outputData(nChans * nFeatures, nHops);
    double     sampleRate{0};
    // Copy input data
    for (index i = 0; i < nChans; ++i)
    {
      for (index j = 0; j < asSigned(inputBuffers.size()); ++j)
      {
        BufferAdaptor::ReadAccess thisInput(inputBuffers[asUnsigned(j)].buffer);
        if (i == 0 && j == 0) sampleRate = thisInput.sampleRate();
        inputData[asUnsigned(j)].row(i)(Slice(0, nFrames)) =
            thisInput.samps(inputBuffers[asUnsigned(j)].startFrame, nFrames,
                            inputBuffers[asUnsigned(j)].startChan + i);
      }
    }
    FluidTask*   task = c.task();
    FluidContext dummyContext;
    for (index i = 0; i < nChans; ++i)
    {
      client.reset();
      for (index j = 0; j < nHops; ++j)
      {
        index                       t = j * controlRate;
        std::vector<HostVectorView> inputs;
        inputs.reserve(inputBuffers.size());
        std::vector<HostVectorView> outputs;
        outputs.reserve(outputBuffers.size());
        for (index k = 0; k < asSigned(inputBuffers.size()); ++k)
          inputs.emplace_back(
              inputData[asUnsigned(k)].row(i)(Slice(t, controlRate)));

        for (index k = 0; k < nFeatures; ++k)
          outputs.emplace_back(outputData.row(k + i * nFeatures)(Slice(j, 1)));


        client.process(inputs, outputs, dummyContext);

        if (task && !task->processUpdate(j + 1 + (nHops * i),
                                         static_cast<double>(nHops * nChans)))
          break;
      }
    }

    BufferAdaptor::Access thisOutput(outputBuffers[0]);
    Result resizeResult = thisOutput.resize(nHops - 1, nChans * nFeatures,
                                            sampleRate / controlRate);
    if (!resizeResult.ok()) return resizeResult;

    for (index i = 0; i < nFeatures; ++i)
    {
      for (index j = 0; j < nChans; ++j)
        thisOutput.samps(i + j * nFeatures) =
            outputData.row(i + j * nFeatures)(Slice(1));
    }

    return {};
  }
};


//////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename HostMatrix, typename HostVectorView>
struct Slicing
{
  template <typename Client, typename InputList, typename OutputList>
  static Result process(Client& client, InputList& inputBuffers,
                        OutputList& outputBuffers, index nFrames, index nChans,
                        FluidContext& c)
  {

    assert(inputBuffers.size() == 1);
    assert(outputBuffers.size() == 1);
    index      padding = client.latency();
    HostMatrix monoSource(1, nFrames + padding);

    BufferAdaptor::ReadAccess src(inputBuffers[0].buffer);
    // Make a mono sum;
    for (index i = inputBuffers[0].startChan;
         i < nChans + inputBuffers[0].startChan; ++i)
      monoSource.row(0)(Slice(0, nFrames))
          .apply(src.samps(inputBuffers[0].startFrame, nFrames, i),
                 [](float& x, float y) { x += y; });

    HostMatrix onsetPoints(1, nFrames + padding);

    std::vector<HostVectorView> input{monoSource.row(0)};
    std::vector<HostVectorView> output{onsetPoints.row(0)};

    client.reset();
    client.process(input, output, c);

    if (padding)
    {
      auto paddingAudio = onsetPoints(0, Slice(0, padding));
      auto numNegativeTimeOnsets =
          std::count_if(paddingAudio.begin(), paddingAudio.end(),
                        [](float x) { return x > 0; });

      if (numNegativeTimeOnsets > 0) onsetPoints(0, padding) = 1;
    }

    return impl::spikesToTimes(onsetPoints(0, Slice(padding, nFrames)),
                               outputBuffers[0], 1, inputBuffers[0].startFrame,
                               nFrames, src.sampleRate());
  }
};

} // namespace impl
//////////////////////////////////////////////////////////////////////////////////////////////////////

template <class RTClient, typename Params, Params& PD, index Ins, index Outs>
using NRTStreamAdaptor =
    impl::NRTClientWrapper<impl::Streaming, RTClient, Params, PD, Ins, Outs>;

template <class RTClient, typename Params, Params& PD, index Ins, index Outs>
using NRTSliceAdaptor =
    impl::NRTClientWrapper<impl::Slicing, RTClient, Params, PD, Ins, Outs>;

template <class RTClient, typename Params, Params& PD, index Ins, index Outs>
using NRTControlAdaptor =
    impl::NRTClientWrapper<impl::StreamingControl, RTClient, Params, PD, Ins,
                           Outs>;


//////////////////////////////////////////////////////////////////////////////////////////////////////


template <template <typename T> class RTClient, typename... Args>
auto constexpr makeNRTParams(impl::InputBufferSpec&& in, Args&&... outs)
{
  return impl::joinParameterDescriptors(
      impl::joinParameterDescriptors(
          impl::makeWrapperInputs(in),
          defineParameters(std::forward<Args>(outs)...)),
      RTClient<double>::getParameterDescriptors());
}


//////////////////////////////////////////////////////////////////////////////////////////////////////

template <typename NRTClient>
class NRTThreadingAdaptor : public OfflineIn, public OfflineOut
{
public:
  using ClientPointer = typename std::shared_ptr<NRTClient>;
  using ParamDescType = typename NRTClient::ParamDescType;
  using ParamSetType = typename NRTClient::ParamSetType;
  using ParamSetViewType = typename NRTClient::ParamSetViewType;

  constexpr static ParamDescType& getParameterDescriptors()
  {
    return NRTClient::getParameterDescriptors();
  }

  index audioChannelsIn() const noexcept { return 0; }
  index audioChannelsOut() const noexcept { return 0; }
  index controlChannelsIn() const noexcept { return 0; }
  index controlChannelsOut() const noexcept { return 0; }
  index audioBuffersIn() const noexcept
  {
    return ParamDescType::template NumOf<InputBufferT>();
  }
  index audioBuffersOut() const noexcept
  {
    return ParamDescType::template NumOf<BufferT>();
  }

  NRTThreadingAdaptor(ParamSetType& p)
      : mHostParams{p}, mClient{new NRTClient{mHostParams}}
  {}

  ~NRTThreadingAdaptor()
  {
    if (mThreadedTask)
    {
      mThreadedTask->cancel(true);
      mThreadedTask.release();
    }
  }

  // We need this so we can remake the client when the fake-sr changes in PD
  // (TODO: something better; NRT clients shouldn't assume that the SR is
  // lifetime constant)
  void recreateClient() { mClient.reset(new NRTClient{mHostParams}); }

  Result enqueue(ParamSetType& p)
  {
    if (mThreadedTask && (mSynchronous || !mQueueEnabled))
      return {Result::Status::kError, "already processing"};

    mQueue.push_back(p);

    return {};
  }

  Result process()
  {
    if (mThreadedTask && (mSynchronous || !mQueueEnabled))
      return {Result::Status::kError, "already processing"};

    if (mThreadedTask) return Result();

    Result result;

    if (mQueue.empty())
      return {Result::Status::kWarning, "Process() called on empty queue"};

    mThreadedTask = std::unique_ptr<ThreadedTask>(
        new ThreadedTask(mClient, mQueue.front(), mSynchronous));
    mQueue.pop_front();

    if (mSynchronous)
    {
      result = mThreadedTask->result();
      mThreadedTask = nullptr;
    }

    return result;
  }

  ProcessState checkProgress(Result& result)
  {
    if (mThreadedTask)
    {
      auto state = mThreadedTask->checkProgress(result);

      if (state == kDone)
      {
        if (!mQueue.empty())
        {
          mThreadedTask = std::unique_ptr<ThreadedTask>(
              new ThreadedTask(mClient, mQueue.front(), false));
          mQueue.pop_front();
          state = kDoneStillProcessing;
          mThreadedTask->mState = kDoneStillProcessing;
        }
        else
        {
          mThreadedTask = nullptr;
        }
      }

      return state;
    }

    return kNoProcess;
  }

  void setSynchronous(bool synchronous) { mSynchronous = synchronous; }

  void setQueueEnabled(bool queue) { mQueueEnabled = queue; }

  double progress()
  {
    return mThreadedTask ? mThreadedTask->mTask.progress() : 0.0;
  }

  void cancel()
  {
    mQueue.clear();

    if (mThreadedTask) mThreadedTask->cancel(false);
  }

  bool done() const
  {
    return mThreadedTask ? (mThreadedTask->mState == kDone ||
                            mThreadedTask->mState == kDoneStillProcessing)
                         : false;
  }

  ProcessState state() const
  {
    return mThreadedTask ? mThreadedTask->mState : kNoProcess;
  }


private:
  struct ThreadedTask
  {
    template <size_t N, typename T>
    struct BufferCopy
    {
      void operator()(typename T::type& param)
      {
        if (param) param = typename T::type(new MemoryBufferAdaptor(param));
      }
    };

    template <size_t N, typename T>
    struct BufferCopyBack
    {
      void operator()(typename T::type& param)
      {
        if (param)
          static_cast<MemoryBufferAdaptor*>(param.get())->copyToOrigin();
      }
    };

    template <size_t N, typename T>
    struct BufferDelete
    {
      void operator()(typename T::type& param) { param.reset(); }
    };

    ThreadedTask(ClientPointer client, ParamSetType& hostParams,
                 bool synchronous)
        : mProcessParams(hostParams), mState(kNoProcess),
          mClient(client), mContext{mTask}
    {

      assert(mClient.get() != nullptr); // right?

      std::promise<Result> resultPromise;
      mFutureResult = resultPromise.get_future();

      if (synchronous)
      {
        mClient->setParams(hostParams);
        process(std::move(resultPromise));
      }
      else
      {
        auto entry = [](ThreadedTask* owner, std::promise<Result> result) {
          owner->process(std::move(result));
        };
        mProcessParams.template forEachParamType<BufferT, BufferCopy>();
        mProcessParams.template forEachParamType<InputBufferT, BufferCopy>();
        mClient->setParams(mProcessParams);
        mState = kProcessing;
        mThread = std::thread(entry, this, std::move(resultPromise));
      }
    }

    Result result() { return mFutureResult.get(); }

    void process(std::promise<Result> result)
    {
      assert(mClient.get() != nullptr); // right?

      mState = kProcessing;
      Result r = mClient->process(mContext);
      result.set_value(r);
      mState = kDone;

      if (mDetached) delete this;
    }

    void cancel(bool detach)
    {
      mTask.cancel();

      mDetached = detach;

      if (detach && mThread.joinable()) mThread.detach();
    }

    ProcessState checkProgress(Result& result)
    {
      ProcessState state = mState;

      if (state == kDone)
      {
        if (mThread.get_id() != std::thread::id())
        {
          result = mFutureResult.get();
          mThread.join();
        }

        if (!mTask.cancelled())
        {
          if (result.status() != Result::Status::kError)
            mProcessParams.template forEachParamType<BufferT, BufferCopyBack>();
        }
        else
          result = {Result::Status::kCancelled, ""};

        mProcessParams.template forEachParamType<BufferT, BufferDelete>();
        mState = kNoProcess;
      }

      return state;
    }

    ParamSetType        mProcessParams;
    ProcessState        mState;
    std::thread         mThread;
    std::future<Result> mFutureResult;
    Result              mResult;
    ClientPointer       mClient;
    FluidTask           mTask;
    FluidContext        mContext;
    bool                mDetached = false;
  };

  ParamSetType                  mHostParams;
  std::deque<ParamSetType>      mQueue;
  bool                          mSynchronous = false;
  bool                          mQueueEnabled = false;
  std::unique_ptr<ThreadedTask> mThreadedTask;
  ClientPointer                 mClient;
};

} // namespace client
} // namespace fluid
