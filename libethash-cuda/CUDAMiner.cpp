/*
This file is part of ethminer.

ethminer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

ethminer is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with ethminer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <libethcore/Farm.h>
#include <ethash/ethash.hpp>

#include "CUDAMiner.h"

using namespace std;
using namespace dev;
using namespace eth;

unsigned CUDAMiner::s_numInstances = 0;

vector<int> CUDAMiner::s_devices(MAX_MINERS, -1);

struct CUDAChannel : public LogChannel
{
    static const char* name() { return EthOrange "cu"; }
    static const int verbosity = 2;
};
#define cudalog clog(CUDAChannel)

CUDAMiner::CUDAMiner(unsigned _index)
  : Miner("cuda-", _index),
    m_io_strand(g_io_service),
    m_light(getNumDevices()),
    m_batch_size(s_gridSize * s_blockSize),
    m_streams_batch_size(s_gridSize * s_blockSize * s_numStreams)
{}

CUDAMiner::~CUDAMiner()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cudalog, "cuda-" << m_index << " CUDAMiner::~CUDAMiner() begin");
    stopWorking();
    kick_miner();
    DEV_BUILD_LOG_PROGRAMFLOW(cudalog, "cuda-" << m_index << " CUDAMiner::~CUDAMiner() end");
}

unsigned CUDAMiner::s_parallelHash = 4;
unsigned CUDAMiner::s_blockSize = CUDAMiner::c_defaultBlockSize;
unsigned CUDAMiner::s_gridSize = CUDAMiner::c_defaultGridSize;
unsigned CUDAMiner::s_numStreams = CUDAMiner::c_defaultNumStreams;
unsigned CUDAMiner::s_scheduleFlag = 0;

bool CUDAMiner::init(int epoch)
{
    // When loading of DAG is sequential wait for
    // this instance to become current
    if (s_dagLoadMode == DAG_LOAD_MODE_SEQUENTIAL)
    {
        while (s_dagLoadIndex < m_index)
        {
            boost::system_time const timeout =
                boost::get_system_time() + boost::posix_time::seconds(3);
            boost::mutex::scoped_lock l(x_work);
            m_dag_loaded_signal.timed_wait(l, timeout);
        }
        if (shouldStop())
        {
            cudalog << "Exiting ...";
            return false;
        }
    }

    unsigned device = s_devices[m_index] > -1 ? s_devices[m_index] : m_index;

    cnote << "Initialising miner " << m_index;

    auto numDevices = getNumDevices();
    if (numDevices == 0)
        return false;

    // use selected device
    m_device_num = device < numDevices - 1 ? device : numDevices - 1;
    m_hwmoninfo.deviceType = HwMonitorInfoType::NVIDIA;
    m_hwmoninfo.indexSource = HwMonitorIndexSource::CUDA;
    m_hwmoninfo.deviceIndex = m_device_num;

    cudaDeviceProp device_props;
    CUDA_SAFE_CALL(cudaGetDeviceProperties(&device_props, m_device_num));

    cudalog << "Using device: " << device_props.name
            << " (Compute " + to_string(device_props.major) + "." + to_string(device_props.minor) +
                   ")";

    const auto& context = ethash::get_global_epoch_context(epoch);
    const auto lightNumItems = context.light_cache_num_items;
    const auto lightSize = ethash::get_light_cache_size(lightNumItems);
    const auto dagNumItems = context.full_dataset_num_items;
    const auto dagSize = ethash::get_full_dataset_size(dagNumItems);

    CUDA_SAFE_CALL(cudaSetDevice(m_device_num));
    cudalog << "Set Device to current";
    if (dagNumItems != m_dag_size || !m_dag)
    {
        // Check whether the current device has sufficient memory every time we recreate the dag
        if (device_props.totalGlobalMem < dagSize)
        {
            cudalog << "CUDA device " << string(device_props.name)
                    << " has insufficient GPU memory. "
                    << FormattedMemSize(device_props.totalGlobalMem) << " of memory found, "
                    << FormattedMemSize(dagSize) << " of memory required";
            return false;
        }
        // We need to reset the device and recreate the dag
        cudalog << "Resetting device";
        CUDA_SAFE_CALL(cudaDeviceReset());
        CUDA_SAFE_CALL(cudaSetDeviceFlags(s_scheduleFlag));
        CUDA_SAFE_CALL(cudaDeviceSetCacheConfig(cudaFuncCachePreferL1));
        // We need to reset the light and the Dag for the following code to reallocate
        // since cudaDeviceReset() frees all previous allocated memory
        m_light[m_device_num] = nullptr;
        m_dag = nullptr;
    }
    // create buffer for cache
    hash128_t* dag = m_dag;
    hash64_t* light = m_light[m_device_num];

    if (!light)
    {
        cudalog << "Allocating light with size: " << FormattedMemSize(lightSize);
        CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&light), lightSize));
    }
    // copy lightData to device
    CUDA_SAFE_CALL(cudaMemcpy(
        reinterpret_cast<void*>(light), context.light_cache, lightSize, cudaMemcpyHostToDevice));
    m_light[m_device_num] = light;

    if (dagNumItems != m_dag_size || !dag)  // create buffer for dag
        CUDA_SAFE_CALL(cudaMalloc(reinterpret_cast<void**>(&dag), dagSize));

    set_constants(dag, dagNumItems, light, lightNumItems);  // in ethash_cuda_miner_kernel.cu

    if (dagNumItems != m_dag_size || !dag)
    {
        // create mining buffers
        cudalog << "Generating mining buffers";
        for (unsigned i = 0; i != s_numStreams; ++i)
        {
            CUDA_SAFE_CALL(cudaMallocHost(&m_search_buf[i], sizeof(Search_results)));
            CUDA_SAFE_CALL(cudaStreamCreateWithFlags(&m_streams[i], cudaStreamNonBlocking));
        }

        m_current_target = 0;

        cudalog << "Generating DAG for GPU #" << m_device_num
                << " with dagSize: " << FormattedMemSize(dagSize) << " ("
                << FormattedMemSize(device_props.totalGlobalMem - dagSize - lightSize) << " left)";
        auto startDAG = std::chrono::steady_clock::now();

        ethash_generate_dag(dagSize, s_gridSize, s_blockSize, m_streams[0]);

        cudalog << "Generated DAG for GPU" << m_device_num << " in: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - startDAG)
                       .count()
                << " ms.";

    }

    m_dag = dag;
    m_dag_size = dagNumItems;

    s_dagLoadIndex++;
    if (s_dagLoadMode == DAG_LOAD_MODE_SEQUENTIAL)
        m_dag_loaded_signal.notify_all();

    return true;
}

void CUDAMiner::workLoop()
{
    WorkPackage current;
    current.header = h256();

    m_search_buf.resize(s_numStreams);
    m_streams.resize(s_numStreams);

    try
    {
        while (!shouldStop())
        {

            // Wait for work or 3 seconds (whichever the first)
            const WorkPackage w = work();
            if (!w)
            {
                boost::system_time const timeout =
                    boost::get_system_time() + boost::posix_time::seconds(3);
                boost::mutex::scoped_lock l(x_work);
                m_new_work_signal.timed_wait(l, timeout);
                continue;
            }

            // Epoch change ?
            if (current.epoch != w.epoch)
            {
                if (!init(w.epoch))
                    break;  // TODO this will simply exit the thread

                // As DAG generation takes a while we need to
                // ensure we're on latest job, not on the one
                // which triggered the epoch change
                current = w;
                continue;
            }

            // Persist most recent job.
            // Job's differences should be handled at higher level
            current = w;
            uint64_t upper64OfBoundary = (uint64_t)(u64)((u256)current.boundary >> 192);

            // Eventually start searching
            search(current.header.data(), upper64OfBoundary, current.startNonce, w);
        }

        // Reset miner and stop working
        CUDA_SAFE_CALL(cudaDeviceReset());
    }
    catch (cuda_runtime_error const& _e)
    {
        string _what = "GPU error: ";
        _what.append(_e.what());
        throw std::runtime_error(_what);
    }
}

void CUDAMiner::kick_miner()
{
    m_new_work.store(true, std::memory_order_relaxed);
    m_new_work_signal.notify_one();
}

void CUDAMiner::setNumInstances(unsigned _instances)
{
    s_numInstances = std::min<unsigned>(_instances, getNumDevices());
}

void CUDAMiner::setDevices(const vector<unsigned>& _devices, unsigned _selectedDeviceCount)
{
    for (unsigned i = 0; i < _selectedDeviceCount; i++)
        s_devices[i] = _devices[i];
}

unsigned CUDAMiner::getNumDevices()
{
    int deviceCount;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err == cudaSuccess)
        return deviceCount;

    if (err == cudaErrorInsufficientDriver)
    {
        int driverVersion = 0;
        cudaDriverGetVersion(&driverVersion);
        if (driverVersion == 0)
            throw std::runtime_error{"No CUDA driver found"};
        throw std::runtime_error{"Insufficient CUDA driver: " + std::to_string(driverVersion)};
    }

    throw std::runtime_error{cudaGetErrorString(err)};
}

void CUDAMiner::listDevices()
{
    try
    {
        cout << "List of CUDA capable devices" << endl
             << setw(3) << setiosflags(ios::left) << "Id" << setw(11) << "Pci Id" << setw(23)
             << "Name" << setw(5) << "SM" << resetiosflags(ios::left) << setw(10) << "Mem" << endl;

        int numDevices = getNumDevices();
        for (int i = 0; i < numDevices; ++i)
        {
            cudaDeviceProp props;
            CUDA_SAFE_CALL(cudaGetDeviceProperties(&props, i));

            cout << setw(3) << setiosflags(ios::left) << to_string(i) << resetiosflags(ios::left)
                 << setw(4) << setfill('0') << hex << props.pciDomainID << ":" << setw(2)
                 << props.pciBusID << ":" << setw(2) << props.pciDeviceID << " " << setw(23)
                 << setiosflags(ios::left) << setfill(' ') << string(props.name) << setw(5)
                 << (to_string(props.major) + "." + to_string(props.minor))
                 << resetiosflags(ios::left) << setw(10) << FormattedMemSize(props.totalGlobalMem)
                 << endl;

            //

            // cout << "[" + to_string(i) + "] " + string(props.name) + "\n";
            // cout << "\tCompute version: " + to_string(props.major) + "." + to_string(props.minor)
            // +
            //            "\n";
            // cout << "\tcudaDeviceProp::totalGlobalMem: " + to_string(props.totalGlobalMem) +
            // "\n"; cout << "\tPci: " << setw(4) << setfill('0') << hex << props.pciDomainID << ':'
            //     << setw(2) << props.pciBusID << ':' << setw(2) << props.pciDeviceID << '\n';
        }
    }
    catch (std::runtime_error const& err)
    {
        string _what = "CUDA error: ";
        _what.append(err.what());
        throw std::runtime_error(_what);
    }
}

unsigned const CUDAMiner::c_defaultBlockSize = 128;
unsigned const CUDAMiner::c_defaultGridSize = 8192;  // * CL_DEFAULT_LOCAL_WORK_SIZE
unsigned const CUDAMiner::c_defaultNumStreams = 2;

bool CUDAMiner::configureGPU(unsigned _blockSize, unsigned _gridSize, unsigned _numStreams,
    unsigned _parallelHash, unsigned _scheduleFlag, unsigned _dagLoadMode)
{
    s_dagLoadMode = _dagLoadMode;
    s_blockSize = _blockSize;
    s_gridSize = _gridSize;
    s_numStreams = _numStreams;
    s_scheduleFlag = _scheduleFlag;
    s_parallelHash = _parallelHash;

    cudalog << "Using grid size: " << s_gridSize << ", block size: " << s_blockSize
            << ", streams: " << s_numStreams << " parallel hashes : " << s_parallelHash;

    return true;
}

void CUDAMiner::search(
    uint8_t const* header, uint64_t target, uint64_t start_nonce, const dev::eth::WorkPackage& w)
{
    
    set_header(*reinterpret_cast<hash32_t const*>(header));
    if (m_current_target != target)
    {
        set_target(target);
        m_current_target = target;
    }

    // prime each stream, clear search result buffers and start the search
    uint32_t current_index;
    for (current_index = 0;
         current_index < s_numStreams;
         current_index++, start_nonce += m_batch_size)
    {
        cudaStream_t stream = m_streams[current_index];
        volatile Search_results& buffer(*m_search_buf[current_index]);
        buffer.count = 0;

        // Run the batch for this stream
        run_ethash_search(s_gridSize, s_blockSize, stream, &buffer, start_nonce, s_parallelHash);
    }

    // process stream batches until we get new work.
    bool done = false;


    while (!done)
    {

        // Exit next time around if there's new work awaiting
        bool t = true;
        done = m_new_work.compare_exchange_strong(t, false);

        // Check on every batch if we need to suspend mining
        if (!done)
            done = paused();

        // This inner loop will process each cuda stream individually
        for (current_index = 0;
             current_index < s_numStreams;
             current_index++, start_nonce += m_batch_size)
        {
            // Each pass of this loop will wait for a stream to exit,
            // save any found solutions, then restart the stream
            // on the next group of nonces.
            cudaStream_t stream = m_streams[current_index];

            // Wait for the stream complete
            CUDA_SAFE_CALL(cudaStreamSynchronize(stream));

            if (shouldStop())
            {
                m_new_work.store(false, std::memory_order_relaxed);
                done = true;
            }

            // Detect solutions in current stream's solution buffer
            volatile Search_results& buffer(*m_search_buf[current_index]);
            uint32_t found_count = std::min((unsigned)buffer.count, MAX_SEARCH_RESULTS);

            if (found_count)
            {
                buffer.count = 0;
                uint64_t nonce_base = start_nonce - m_streams_batch_size;
                
                // Extract solution and pass to higer level 
                // using io_service as dispatcher

                for (uint32_t i = 0; i < found_count; i++)
                {
                    h256 mix;
                    uint64_t nonce = nonce_base + buffer.result[i].gid;
                    memcpy(mix.data(), (void*)&buffer.result[i].mix, sizeof(buffer.result[i].mix));
                    auto sol =
                        Solution{nonce, mix, w, std::chrono::steady_clock::now(), m_index};

                        cnote << "Sol: " << EthWhite "0x" << toHex(sol.nonce) << EthReset;

                    g_io_service.post(
                        m_io_strand.wrap(boost::bind(&Farm::submitProof, &Farm::f(), sol)));
                }
            }

            // restart the stream on the next batch of nonces
            // unless we are done for this round.
            if (!done)
                run_ethash_search(
                    s_gridSize, s_blockSize, stream, &buffer, start_nonce, s_parallelHash);

        }

        // Update the hash rate
        updateHashRate(m_batch_size, s_numStreams);

        // Bail out if it's shutdown time
        if (shouldStop())
        {
            m_new_work.store(false, std::memory_order_relaxed);
            break;
        }

    }

#ifdef DEV_BUILD
    // Optionally log job switch time
    if (!shouldStop() && (g_logOptions & LOG_SWITCH))
        cudalog << "Switch time: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - m_workSwitchStart)
                       .count()
                << " ms.";
#endif
}
