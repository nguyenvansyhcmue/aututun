#include <signalsmith-stretch/signalsmith-stretch.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <fstream>
#include <map>

namespace { using Contour = std::map<int64_t, double>;
Contour read (const char* path) { Contour c; std::ifstream s(path); std::string l; std::getline(s,l); while (std::getline(s,l)) { auto a=l.find(','), b=l.find(',',a+1); if(a==std::string::npos||b==std::string::npos) continue; auto v=l.substr(a+1,b-a-1); if(v!="nan") c.emplace(std::stoll(l.substr(0,a)),std::stod(v)); } return c; }
double at (const Contour& c, int64_t x) { auto hi=c.lower_bound(x); if(hi==c.end()||hi==c.begin()) return hi==c.end()?std::numeric_limits<double>::quiet_NaN():hi->second; auto lo=std::prev(hi); auto t=std::clamp(double(x-lo->first)/double(hi->first-lo->first),0.0,1.0); return lo->second+(hi->second-lo->second)*t; }
}
int main (int argc, char* argv[]) {
    if (argc != 5) return 2;
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(argv[1])));
    auto raw=read(argv[2]), target=read(argv[3]); if(!reader||raw.empty()||target.empty()) return 3;
    auto stream=juce::File(argv[4]).createOutputStream(); juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.get(),reader->sampleRate,1,24,{},0)); if(!writer) return 4; stream.release();
    constexpr int block=128; signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.configure(1, 1024, 128, true); stretch.setFormantBase(200.0/reader->sampleRate); stretch.reset();
    juce::AudioBuffer<float> in(1,block), out(1,block); const float* inputs[]{in.getReadPointer(0)}; float* outputs[]{out.getWritePointer(0)};
    struct Input { const float* const* v; const float* operator[](int i) const { return v[i]; } } input{inputs}; struct Output { float* const* v; float* operator[](int i) const { return v[i]; } } output{outputs};
    for (int64_t p=0;p<reader->lengthInSamples;p+=block) { auto n=int(juce::jmin<int64_t>(block,reader->lengthInSamples-p)); in.clear(); out.clear(); reader->read(&in,0,n,p,true,true); auto f=at(raw,p+block/2), g=at(target,p+block/2); auto ratio=std::isfinite(f)&&std::isfinite(g)&&f>0&&g>0?std::clamp(g/f,0.90,1.10):1.0; stretch.setTransposeFactor(float(ratio)); stretch.process(input,n,output,n); writer->writeFromAudioSampleBuffer(out,0,n); }
}
