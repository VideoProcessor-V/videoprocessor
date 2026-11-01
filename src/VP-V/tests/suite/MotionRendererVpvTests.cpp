// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// VP-V tests split out of Bill's MotionRendererTests.cpp by tools/m2/move_tests.py.
#include "pch.h"
#include "CppUnitTest.h"
#include <vprenderer/TemporalRenderer.h>
#include <vprenderer/NativeStatsOverlayPlacement.h>
#include <vprenderer/DisplayFrameCache.h>
#include <vprenderer/DisplayVsyncClock.h>
#include <vprenderer/InputAnalysisCadence.h>
#include <vprenderer/SourceUploadBatch.h>
#include <vprenderer/PresentationAnalysisBudget.h>
#include <vprenderer/AnalysisPerformanceWindow.h>
#include <vprenderer/OptionalAnalysisWorker.h>
#include <vprenderer/AlphaPresentationTelemetry.h>
#include <future>
#include <vprenderer/NlsPipelinePreparation.h>
#include <vprenderer/SubtitlePipelinePreparation.h>
#include <vprenderer/TemporalPresentationLedger.h>
#include <SubtitleCutPasteShader.h>
#include <vprenderer/LibplaceboRenderParameters.h>
#include <vprenderer/ExtendedRange.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/utils/upload.h>
#include <array>
#include <fstream>
#include <cstdint>
#include <type_traits>
#include "RendererProfileConfig.h"
#include <vector>
using namespace Microsoft::VisualStudio::CppUnitTestFramework;
namespace {
struct GpuFixture {
    SubtitlePreparationRenderer preparation;
    pl_d3d11 device=nullptr;pl_renderer renderer=nullptr;pl_tex source=nullptr,target=nullptr,clean=nullptr,layer=nullptr,luma=nullptr,chroma=nullptr;
    pl_frame input{},output{},cleanInput{},p010Input{};TemporalRenderer temporal;
    GpuFixture(){
        pl_d3d11_params d{};d.allow_software=true;device=pl_d3d11_create(nullptr,&d);
        Assert::IsTrue(device!=nullptr,L"D3D11 fixture unavailable");renderer=pl_renderer_create(nullptr,device->gpu);
        pl_tex_params params{};params.w=64;params.h=64;
        params.format=pl_find_named_fmt(device->gpu,"rgba8");
        Assert::IsTrue(params.format!=nullptr,L"RGBA8 missing");
        params.sampleable=true;params.renderable=true;params.blit_src=true;params.blit_dst=true;
        params.host_writable=true;params.host_readable=true;
        source=pl_tex_create(device->gpu,&params);target=pl_tex_create(device->gpu,&params);
        const pl_fmt half=pl_find_named_fmt(device->gpu,"rgba16f");
        Assert::IsTrue(half!=nullptr,L"RGBA16F missing");params.format=half;
        clean=pl_tex_create(device->gpu,&params);layer=pl_tex_create(device->gpu,&params);
        Assert::IsTrue(clean&&layer,L"Composition texture allocation failed");
        Assert::IsTrue(source&&target,L"Texture allocation failed");
        input.num_planes=1;input.planes[0].texture=source;input.planes[0].components=4;
        for(int c=0;c<4;++c)input.planes[0].component_mapping[c]=c;
        input.repr=pl_color_repr_rgb;input.color=pl_color_space_srgb;input.crop={0,0,64,64};
        output=input;output.planes[0].texture=target;
        cleanInput=input;cleanInput.planes[0].texture=clean;cleanInput.repr.sys=PL_COLOR_SYSTEM_RGB;
        cleanInput.repr.levels=PL_COLOR_LEVELS_FULL;cleanInput.repr.alpha=PL_ALPHA_NONE;
    }
    ~GpuFixture(){temporal.Clear();preparation.Reset();if(device){pl_tex_destroy(device->gpu,&source);pl_tex_destroy(device->gpu,&target);pl_tex_destroy(device->gpu,&clean);pl_tex_destroy(device->gpu,&layer);pl_tex_destroy(device->gpu,&luma);pl_tex_destroy(device->gpu,&chroma);pl_renderer_destroy(&renderer);pl_d3d11_destroy(&device);}}
    void Upload(int gray,bool fields=false){
        std::vector<unsigned char> pixels(64*64*4,255);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x){const size_t o=(y*64+x)*4;
            pixels[o]=fields?(y%2?0:255):static_cast<unsigned char>(gray);
            pixels[o+1]=fields?0:static_cast<unsigned char>(gray);
            pixels[o+2]=fields?(y%2?255:0):static_cast<unsigned char>(gray);
        }
        pl_tex_transfer_params upload{};upload.tex=source;upload.ptr=pixels.data();
        Assert::IsTrue(pl_tex_upload(device->gpu,&upload));
    }
    void CreateP010AlternatingChroma(bool impulse=false){
        std::vector<uint16_t> y(64*64,uint16_t(500<<6));
        std::vector<uint16_t> uv(32*32*2);
        for(int row=0;row<32;++row)for(int col=0;col<32;++col){
            const size_t o=(row*32+col)*2;
            const int u=impulse?(row==16?900:512):(row%2?300:700);
            const int v=impulse?(row==16?100:512):(row%2?700:300);
            uv[o]=uint16_t(u<<6);uv[o+1]=uint16_t(v<<6);
        }
        pl_tex_params p{};p.w=p.h=64;p.sampleable=true;p.blit_src=true;p.blit_dst=true;
        p.format=pl_find_fmt(device->gpu,PL_FMT_UNORM,1,16,16,
            static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE|PL_FMT_CAP_BLITTABLE));
        Assert::IsTrue(p.format!=nullptr,L"P010 luma format unavailable");
        p.initial_data=y.data();luma=pl_tex_create(device->gpu,&p);
        Assert::IsTrue(luma!=nullptr,L"P010 luma texture unavailable");
        p.w=p.h=32;
        p.format=pl_find_fmt(device->gpu,PL_FMT_UNORM,2,16,16,
            static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE|PL_FMT_CAP_BLITTABLE));
        Assert::IsTrue(p.format!=nullptr,L"P010 chroma format unavailable");
        p.initial_data=uv.data();chroma=pl_tex_create(device->gpu,&p);
        Assert::IsTrue(chroma!=nullptr,L"P010 chroma texture unavailable");
        p010Input.num_planes=2;
        p010Input.planes[0].texture=luma;p010Input.planes[0].components=1;
        p010Input.planes[0].component_mapping[0]=0;
        p010Input.planes[1].texture=chroma;p010Input.planes[1].components=2;
        p010Input.planes[1].component_mapping[0]=1;
        p010Input.planes[1].component_mapping[1]=2;
        p010Input.repr.sys=PL_COLOR_SYSTEM_BT_2020_NC;
        p010Input.repr.levels=PL_COLOR_LEVELS_LIMITED;
        p010Input.repr.alpha=PL_ALPHA_NONE;
        p010Input.repr.bits.sample_depth=16;p010Input.repr.bits.color_depth=10;
        p010Input.repr.bits.bit_shift=6;
        p010Input.color=pl_color_space_srgb;
        p010Input.color.transfer=PL_COLOR_TRC_PQ;
        p010Input.color.primaries=PL_COLOR_PRIM_BT_2020;
        p010Input.color.hdr.min_luma=0.005f;p010Input.color.hdr.max_luma=1000.0f;
        p010Input.crop={0,0,64,64};
    }
    void UploadFloatPixels(pl_tex texture,std::vector<float>& values){
        const pl_fmt format=texture->params.format;
        Assert::IsTrue(format->num_components==4,L"Expected a four-channel texture");
        pl_tex_transfer_params upload{};upload.tex=texture;
        if(format->texel_size==16 && format->host_bits[0]==32){
            upload.ptr=values.data();
            Assert::IsTrue(pl_tex_upload(device->gpu,&upload),L"Float32 host upload failed");
        }else if(format->texel_size==8 && format->host_bits[0]==16){
            std::vector<uint16_t> half(values.size());
            for(size_t i=0;i<values.size();++i)half[i]=values[i]>=1.0f?uint16_t(0x3c00):uint16_t(0);
            upload.ptr=half.data();
            Assert::IsTrue(pl_tex_upload(device->gpu,&upload),L"Float16 host upload failed");
        }else Assert::Fail(L"Unsupported RGBA16F host transfer layout");
    }
    void UploadComposition(bool white,int glyphColor){
        std::vector<float> background(64*64*4);
        std::vector<float> subtitle(64*64*4);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x){
            const size_t o=(y*64+x)*4;
            background[o]=background[o+1]=background[o+2]=white?1.0f:0.0f;
            background[o+3]=1.0f;
            if(x>=24&&x<40&&y>=24&&y<40&&glyphColor){
                subtitle[o+(glyphColor==1?0:2)]=1.0f;
                subtitle[o+3]=1.0f;
            }
        }
        UploadFloatPixels(clean,background);
        UploadFloatPixels(layer,subtitle);
    }
    void UploadFieldComposition(){
        std::vector<float> background(64*64*4);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x){
            const size_t o=(y*64+x)*4;
            background[o+(y%2?2:0)]=1.0f;
            background[o+3]=1.0f;
        }
        UploadFloatPixels(clean,background);
    }
    std::vector<unsigned char> ReadPixels(){
        std::vector<unsigned char> pixels(64*64*4);
        pl_tex_transfer_params read{};read.tex=target;read.ptr=pixels.data();
        Assert::IsTrue(pl_tex_download(device->gpu,&read));
        return pixels;
    }
    std::array<int,3> Read(int x,int y){
        std::vector<unsigned char> pixels(64*64*4);pl_tex_transfer_params read{};read.tex=target;read.ptr=pixels.data();
        Assert::IsTrue(pl_tex_download(device->gpu,&read));const size_t o=(y*64+x)*4;
        return {pixels[o],pixels[o+1],pixels[o+2]};
    }
    static std::wstring PixelText(const std::array<int,3>& p){
        return L"R="+std::to_wstring(p[0])+L" G="+std::to_wstring(p[1])+L" B="+std::to_wstring(p[2]);
    }
    std::array<int,3> Draw(){
        auto params=pl_render_default_params;params.dither_params=nullptr;params.deband_params=nullptr;params.peak_detect_params=nullptr;
        Assert::IsTrue(temporal.Render(renderer,input,output,params),L"Motion render failed");
        std::vector<unsigned char> pixels(64*64*4);pl_tex_transfer_params read{};read.tex=target;read.ptr=pixels.data();
        Assert::IsTrue(pl_tex_download(device->gpu,&read));const size_t o=(32*64+32)*4;
        return {pixels[o],pixels[o+1],pixels[o+2]};
    }
};
}
TEST_CLASS(MotionRendererTestsVpv){public:

                            
    // Hunt 2 (08 #1): a deinterlaced frame is drawn from the retained copy; with
    // Extended Range the current frame's colour is relabelled after Capture, so
    // the copy must carry that label (else the ER hooks and libplacebo both convert).
    TEST_METHOD(HistoryFramesTakeThePreparedColourWhenGiven){
        // REHEARSAL: adapted to Bill TemporalRenderer API. HistoryFrame() no longer exists
        // (the override is applied inside ProcessSource); the behaviour is covered by the two
        // GPU tests below, so this only checks the colour-space comparison they rely on.
        pl_color_space prepared{};prepared.primaries=PL_COLOR_PRIM_BT_2020;prepared.transfer=PL_COLOR_TRC_GAMMA22;
        pl_color_space srgb=pl_color_space_srgb;
        Assert::IsTrue(pl_color_space_equal(&prepared,&prepared));
        Assert::IsFalse(pl_color_space_equal(&srgb,&prepared));
    }
    TEST_METHOD(DeinterlacedHistoryIsConvertedOnceWithThePreparedColour){
        GpuFixture g;MotionConfig::Settings settings;settings.deinterlace="force";settings.algorithm="bob";settings.cadence="frame";
        for(uint64_t i=0;i<3;++i){g.Upload(128);g.temporal.Begin(settings,30,60,false,1,1,i+1,double(i)/30,false);Assert::IsTrue(g.temporal.Capture(g.device->gpu,g.input,i+1));}
        Assert::IsTrue(g.temporal.Active()&&!g.temporal.Mixing());
        auto params=pl_render_default_params;params.dither_params=nullptr;params.deband_params=nullptr;params.peak_detect_params=nullptr;
        auto read=[&]{std::vector<unsigned char> pixels(64*64*4);pl_tex_transfer_params r{};r.tex=g.target;r.ptr=pixels.data();
            Assert::IsTrue(pl_tex_download(g.device->gpu,&r));const size_t o=(32*64+32)*4;return std::array<int,3>{pixels[o],pixels[o+1],pixels[o+2]};};
        // The current frame relabelled after Capture (as ExtendedRange::Prepare does): linear light.
        pl_frame relabelled=g.input;relabelled.color.transfer=PL_COLOR_TRC_LINEAR;
        Assert::IsTrue(pl_render_image(g.renderer,&relabelled,&g.output,&params));const auto direct=read();
        g.temporal.outputPts=1./30;
        Assert::IsTrue(g.temporal.Render(g.renderer,relabelled,g.output,params,0,&relabelled.color));const auto once=read();
        Assert::IsTrue(g.temporal.Render(g.renderer,relabelled,g.output,params));const auto captured=read();
        for(int c=0;c<3;++c){
            Assert::IsTrue(std::abs(once[c]-direct[c])<=1,L"history frame not drawn with the prepared colour");
            Assert::IsTrue(std::abs(captured[c]-direct[c])>=20,L"fixture does not distinguish the labels");
        }
    }
    // Hunt 2 (08 #1) with the real Extended Range hooks: gamma 2.4 source on a
    // gamma 2.2 output (the carry re-encodes). Deinterlaced through the temporal
    // renderer with the prepared colour = the direct single-frame render with the
    // same hooks; with the captured label the gamma is converted twice.
    TEST_METHOD(DeinterlacedExtendedRangeConvertsEachFrameOnce){
        GpuFixture g;MotionConfig::Settings settings;settings.deinterlace="force";settings.algorithm="bob";settings.cadence="frame";
        pl_color_space source{};source.primaries=PL_COLOR_PRIM_BT_709;source.transfer=PL_COLOR_TRC_GAMMA24;
        pl_color_space output=source;output.transfer=PL_COLOR_TRC_GAMMA22;
        g.input.color=source;g.output.color=output;
        for(uint64_t i=0;i<3;++i){g.Upload(128);g.temporal.Begin(settings,30,60,false,1,1,i+1,double(i)/30,false);Assert::IsTrue(g.temporal.Capture(g.device->gpu,g.input,i+1));}
        const ExtendedRange::Decision decision=ExtendedRange::Decide(true,ExtendedRange::Settings{},source,output,nullptr,PL_LUT_UNKNOWN);
        Assert::IsTrue(decision.Active()&&decision.convertGamma);
        ExtendedRange::Hooks hooks;std::vector<const pl_hook*> list;pl_frame prepared=g.input;
        ExtendedRange::Prepare(ExtendedRange::ForTemporal(decision,g.temporal.Mixing()),hooks,prepared.color,output,list,true);
        Assert::IsTrue(pl_color_space_equal(&prepared.color,&output));
        auto params=pl_render_default_params;params.dither_params=nullptr;params.deband_params=nullptr;params.peak_detect_params=nullptr;
        params.hooks=list.data();params.num_hooks=static_cast<int>(list.size());
        auto read=[&]{std::vector<unsigned char> pixels(64*64*4);pl_tex_transfer_params r{};r.tex=g.target;r.ptr=pixels.data();
            Assert::IsTrue(pl_tex_download(g.device->gpu,&r));const size_t o=(32*64+32)*4;return std::array<int,3>{pixels[o],pixels[o+1],pixels[o+2]};};
        Assert::IsTrue(pl_render_image(g.renderer,&prepared,&g.output,&params));const auto direct=read();
        g.temporal.outputPts=1./30;
        Assert::IsTrue(g.temporal.Render(g.renderer,prepared,g.output,params,0,&prepared.color));const auto once=read();
        Assert::IsTrue(g.temporal.Render(g.renderer,prepared,g.output,params));const auto twice=read();
        const auto errors=pl_renderer_get_errors(g.renderer);
        hooks.Release();
        Assert::AreEqual(0,errors.num_disabled_hooks,L"libplacebo disabled an Extended Range hook");
        // 128 in gamma 2.4 shown in gamma 2.2: about 120 once; converted twice it moves further.
        for(int c=0;c<3;++c){
            Assert::IsTrue(std::abs(once[c]-direct[c])<=1,L"deinterlaced frame differs from the single-frame ER render");
            Assert::IsTrue(std::abs(direct[c]-twice[c])>=4,L"the captured label should convert twice (fixture check)");
        }
    }
};
