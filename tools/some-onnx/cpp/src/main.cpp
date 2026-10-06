#define NOMINMAX
#include <onnxruntime_cxx_api.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
constexpr int kRate = 22050;
constexpr int kHop = 256;
constexpr int kWindow = kRate * 2 - kHop;
constexpr int kOverlapFrames = 30;
constexpr int kOverlapSamples = kOverlapFrames * kHop;
constexpr int kWindowFrames = 172;
constexpr int kPitches = 88;
constexpr int kContourBins = 264;

static std::wstring widen(const std::string& text) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 0) throw std::runtime_error("invalid UTF-8 path");
    std::wstring result(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), n);
    return result;
}

static std::string narrow(const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), n, nullptr, nullptr);
    return result;
}

static fs::path executable_dir() {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n == path.size()) throw std::runtime_error("cannot locate executable");
    path.resize(n);
    return fs::path(path).parent_path();
}

static std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }

static void run_ffmpeg(const fs::path& input, const fs::path& wav) {
    fs::path ffmpeg = L"E:\\ffmpeg-2026-08-06-git-95c43d7df7-full_build\\bin\\ffmpeg.exe";
    if (!fs::exists(ffmpeg)) ffmpeg = L"ffmpeg.exe";
    std::wstring command = quote(ffmpeg.wstring()) + L" -y -v error -i " + quote(input.wstring()) +
        L" -ar 22050 -ac 1 -c:a pcm_f32le " + quote(wav.wstring());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        throw std::runtime_error("failed to launch ffmpeg; pass ffmpeg.exe on PATH");
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) throw std::runtime_error("ffmpeg failed with exit code " + std::to_string(code));
}

static std::vector<float> read_float_wav(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open decoded wav");
    char header[12]; f.read(header, 12);
    if (f.gcount() != 12 || std::string(header, 4) != "RIFF" || std::string(header + 8, 4) != "WAVE")
        throw std::runtime_error("invalid WAV header");
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    std::vector<char> data;
    while (f) {
        char chunk[8]; f.read(chunk, 8);
        if (f.gcount() != 8) break;
        uint32_t size; std::memcpy(&size, chunk + 4, 4);
        if (std::string(chunk, 4) == "fmt ") {
            std::vector<char> fmt(size); f.read(fmt.data(), size);
            std::memcpy(&format, fmt.data(), 2); std::memcpy(&channels, fmt.data() + 2, 2);
            std::memcpy(&rate, fmt.data() + 4, 4); std::memcpy(&bits, fmt.data() + 14, 2);
        } else if (std::string(chunk, 4) == "data") {
            data.resize(size); f.read(data.data(), size);
        } else f.seekg(size, std::ios::cur);
        if (size & 1) f.seekg(1, std::ios::cur);
    }
    if (format != 3 || channels != 1 || rate != kRate || bits != 32 || data.empty())
        throw std::runtime_error("ffmpeg did not produce mono float32 22050Hz WAV");
    std::vector<float> audio(data.size() / sizeof(float));
    std::memcpy(audio.data(), data.data(), data.size());
    return audio;
}

struct Matrix { int cols; std::vector<float> values; };
struct Event { int start; int end; int pitch; float amplitude; };

static float at(const Matrix& m, int row, int col) { return m.values[static_cast<size_t>(row) * m.cols + col]; }
static float& at(Matrix& m, int row, int col) { return m.values[static_cast<size_t>(row) * m.cols + col]; }

static std::vector<Event> decode_notes(Matrix frames, Matrix onsets) {
    const int n = static_cast<int>(frames.values.size() / frames.cols);
    Matrix diffs{frames.cols, std::vector<float>(frames.values.size(), 0)};
    for (int lag = 1; lag <= 5; ++lag) {
        for (int t = lag; t < n; ++t)
            for (int p = 0; p < frames.cols; ++p)
                at(diffs, t, p) = std::min(at(diffs, t, p), at(frames, t, p) - at(frames, t - lag, p));
    }
    float onset_max = *std::max_element(onsets.values.begin(), onsets.values.end());
    float diff_max = *std::max_element(diffs.values.begin(), diffs.values.end());
    if (diff_max > 0) for (size_t i = 0; i < diffs.values.size(); ++i) diffs.values[i] = std::max(0.0f, diffs.values[i]) * onset_max / diff_max;
    for (size_t i = 0; i < onsets.values.size(); ++i) onsets.values[i] = std::max(onsets.values[i], diffs.values[i]);
    for (int t = 0; t < std::min(5, n); ++t) for (int p = 0; p < frames.cols; ++p) at(onsets, t, p) = 0;

    std::vector<std::pair<int,int>> peaks;
    for (int p = 0; p < frames.cols; ++p)
        for (int t = 1; t + 1 < n; ++t)
            if (at(onsets,t,p) > at(onsets,t-1,p) && at(onsets,t,p) > at(onsets,t+1,p) && at(onsets,t,p) >= 0.5f)
                peaks.emplace_back(t,p);
    std::sort(peaks.rbegin(), peaks.rend());
    Matrix remaining = frames;
    std::vector<Event> events;
    auto add = [&](int start, int end, int p) {
        if (end - start <= 11) return;
        float sum = 0; for (int t=start;t<end;++t) sum += at(frames,t,p);
        events.push_back({start,end,p+21,sum/(end-start)});
    };
    auto erase_band = [&](int t, int p) {
        at(remaining,t,p)=0;
        if (p>0) at(remaining,t,p-1)=0;
        if (p<87) at(remaining,t,p+1)=0;
    };
    for (auto [start,p] : peaks) {
        if (start >= n-1) continue;
        int i=start+1,k=0;
        while (i<n-1 && k<11) { if (at(remaining,i,p)<0.3f) ++k; else k=0; ++i; }
        i-=k;
        if (i-start<=11) continue;
        for(int t=start;t<i;++t) erase_band(t,p);
        add(start,i,p);
    }
    while (!remaining.values.empty() && *std::max_element(remaining.values.begin(), remaining.values.end()) > 0.3f) {
        auto it=std::max_element(remaining.values.begin(),remaining.values.end());
        size_t idx=static_cast<size_t>(it-remaining.values.begin()); int mid=static_cast<int>(idx/88), p=static_cast<int>(idx%88);
        *it=0;
        int i=mid+1,k=0;
        while(i<n-1 && k<11){ if(at(remaining,i,p)<0.3f)++k;else k=0; erase_band(i,p); ++i; }
        int end=i-1-k;
        i=mid-1;k=0;
        while(i>0 && k<11){ if(at(remaining,i,p)<0.3f)++k;else k=0; erase_band(i,p); --i; }
        int start=i+1+k;
        if(end-start>11) add(start,end,p);
    }
    return events;
}

static void put16(std::vector<uint8_t>& out, uint16_t v){ out.push_back(v>>8); out.push_back(v&255); }
static void put32(std::vector<uint8_t>& out, uint32_t v){ out.push_back(v>>24);out.push_back(v>>16);out.push_back(v>>8);out.push_back(v); }
static void vlq(std::vector<uint8_t>& out,uint32_t v){ uint8_t b[4];int n=0;b[n++]=v&127;while((v>>=7)){b[n++]=(v&127)|128;}while(n)out.push_back(b[--n]); }
static void midi_text(std::vector<uint8_t>& out,uint8_t type,const std::string& s){out.push_back(0xff);out.push_back(type);vlq(out,static_cast<uint32_t>(s.size()));out.insert(out.end(),s.begin(),s.end());}

static void write_midi(const fs::path& path,const std::vector<Event>& events,int total_frames) {
    struct MidiEvent{uint32_t tick;uint8_t a,b;bool meta;};
    std::vector<MidiEvent> es;
    es.push_back({0,0,0,true});
    const double frame_sec=static_cast<double>(kHop)/kRate;
    auto time_sec=[&](int frame){ double base=frame*frame_sec; double win_offset=(kHop/static_cast<double>(kRate))*(172.0-(kWindow/static_cast<double>(kHop)))+0.0018; return std::max(0.0,base-win_offset*std::floor(frame/172.0)); };
    auto tick=[&](int frame){return static_cast<uint32_t>(std::llround(time_sec(frame)*960.0));};
    for(auto e:events){ es.push_back({tick(e.start),static_cast<uint8_t>(0x90),static_cast<uint8_t>(e.pitch),false}); es.push_back({tick(e.end),static_cast<uint8_t>(0x80),static_cast<uint8_t>(e.pitch),false}); }
    std::stable_sort(es.begin(),es.end(),[](const auto&a,const auto&b){if(a.tick!=b.tick)return a.tick<b.tick;return a.a==0x80 && b.a==0x90;});
    std::vector<uint8_t> track; track.push_back(0); midi_text(track,3,"Basic Pitch"); track.push_back(0); track.push_back(0xff);track.push_back(0x51);track.push_back(3);track.push_back(0x07);track.push_back(0xa1);track.push_back(0x20);
    uint32_t last=0;
    for(auto e:es){if(e.meta)continue;vlq(track,e.tick-last);last=e.tick;track.push_back(e.a);track.push_back(e.b);track.push_back(e.a==0x90?100:0);}
    vlq(track,0);track.push_back(0xff);track.push_back(0x2f);track.push_back(0);
    std::vector<uint8_t> file;file.insert(file.end(),{'M','T','h','d'});put32(file,6);put16(file,0);put16(file,1);put16(file,480);file.insert(file.end(),{'M','T','r','k'});put32(file,static_cast<uint32_t>(track.size()));file.insert(file.end(),track.begin(),track.end());
    std::ofstream f(path,std::ios::binary); if(!f)throw std::runtime_error("cannot write MIDI: "+narrow(path.wstring())); f.write(reinterpret_cast<const char*>(file.data()),file.size());
}

int wmain(int argc,wchar_t** argv){
    try{
        if(argc<2||argc>3){std::wcerr<<L"Usage: audio_to_midi.exe input.mp3 [output.mid]\n";return 2;}
        fs::path input=argv[1], output=argc==3?fs::path(argv[2]):input; if(argc==2)output.replace_extension(L".mid");
        if(!fs::exists(input))throw std::runtime_error("input does not exist: "+narrow(input.wstring()));
        fs::path temp=fs::temp_directory_path()/L"redstone_music_decode.wav";
        run_ffmpeg(input,temp); auto audio=read_float_wav(temp); fs::remove(temp);
        std::vector<float> padded(kOverlapSamples/2,0); padded.insert(padded.end(),audio.begin(),audio.end());
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING,"BasicPitch"); Ort::SessionOptions opts; opts.SetIntraOpNumThreads(8); opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        fs::path model=executable_dir()/L"nmp.onnx"; if(!fs::exists(model)) throw std::runtime_error("nmp.onnx must be next to audio_to_midi.exe");
        Ort::Session session(env,model.c_str(),opts); Ort::AllocatorWithDefaultOptions allocator;
        const char* input_name="serving_default_input_2:0"; const char* output_names[]={"StatefulPartitionedCall:1","StatefulPartitionedCall:2","StatefulPartitionedCall:0"};
        std::vector<float> notes,onsets,contours; const int window_step=kWindow-kOverlapSamples;
        for(size_t start=0;start<padded.size();start+=window_step){
            std::vector<float> window(kWindow,0);size_t count=std::min<size_t>(kWindow,padded.size()-start);std::copy_n(padded.data()+start,count,window.data());
            std::array<int64_t,3> shape{1,kWindow,1};auto mem=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
            auto tensor=Ort::Value::CreateTensor<float>(mem,window.data(),window.size(),shape.data(),shape.size());
            auto result=session.Run(Ort::RunOptions{nullptr},&input_name,&tensor,1,output_names,3);
            auto n=result[0].GetTensorData<float>();auto o=result[1].GetTensorData<float>();auto c=result[2].GetTensorData<float>();
            notes.insert(notes.end(),n,n+kWindowFrames*kPitches);onsets.insert(onsets.end(),o,o+kWindowFrames*kPitches);contours.insert(contours.end(),c,c+kWindowFrames*kContourBins);
        }
        const int original_frames=static_cast<int>(std::floor(audio.size()* (static_cast<double>(kRate)/kHop)/kRate));
        const int drop=kOverlapFrames/2; const int produced=static_cast<int>(notes.size()/(kWindowFrames*kPitches));
        auto unwrap=[&](const std::vector<float>& src,int cols){std::vector<float> out;out.reserve(static_cast<size_t>(produced*kWindowFrames));for(int w=0;w<produced;++w){int begin=w==0?0:drop;int end=w==produced-1?kWindowFrames:kWindowFrames-drop;out.insert(out.end(),src.begin()+static_cast<size_t>(w*kWindowFrames+begin)*cols,src.begin()+static_cast<size_t>(w*kWindowFrames+end)*cols);}if(static_cast<int>(out.size()/cols)>original_frames)out.resize(static_cast<size_t>(original_frames)*cols);return out;};
        Matrix frame{88,unwrap(notes,88)}, onset{88,unwrap(onsets,88)}; auto events=decode_notes(std::move(frame),std::move(onset)); write_midi(output,events,original_frames);
        std::cout<<"Wrote "<<events.size()<<" notes to "<<narrow(output.wstring())<<"\n"; return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
}
