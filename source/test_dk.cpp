#include "framework/CApplication.h"
#include "framework/CMemPool.h"

// C++ standard library headers
#include <array>
#include <optional>

#include "nanovg.h"
#define NANOVG_DK_IMPLEMENTATION
#include "nanovg_dk.h"
#include "demo.h"

#include "debug.hpp"

#ifndef USE_OPENGL
namespace {

    struct Vertex
    {
        float position[3];
        float color[3];
    };

    constexpr std::array VertexAttribState =
    {
        DkVtxAttribState{ 0, 0, offsetof(Vertex, position), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
        DkVtxAttribState{ 0, 0, offsetof(Vertex, color),    DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
    };

    constexpr std::array VertexBufferState =
    {
        DkVtxBufferState{ sizeof(Vertex), 0 },
    };

    constexpr std::array TriangleVertexData =
    {
        Vertex{ { -0.5f, -0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
        Vertex{ {  0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
        Vertex{ {  0.0f,  0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
    };

}

extern "C" u32 __nx_applet_exit_mode;

void OutputDkDebug(void* userData, const char* context, DkResult result, const char* message) 
{
    OutputDebugString("Context: %s\nResult: %d\nMessage: %s\n", context, result, message);
    
    if (result != DkResult_Success) {
        ErrorApplicationConfig ctx;
        errorApplicationCreate(&ctx, context, message);
        errorApplicationSetNumber(&ctx, result);
        errorApplicationShow(&ctx);

        __nx_applet_exit_mode = 1;
        exit(1);
    }
}

class DkTest final : public CApplication
{
    static constexpr unsigned NumFramebuffers = 2;
    uint32_t framebufferWidth = 1280;
    uint32_t framebufferHeight = 720;
    float windowScale = 1.5f;
    static constexpr unsigned StaticCmdSize = 0x1000;

    dk::UniqueDevice device;
    dk::UniqueQueue queue;

    std::optional<CMemPool> pool_images;
    std::optional<CMemPool> pool_code;
    std::optional<CMemPool> pool_data;

    dk::UniqueCmdBuf cmdbuf;

    CShader vertexShader;
    CShader fragmentShader;

    CMemPool::Handle vertexBuffer;

    CMemPool::Handle depthBuffer_mem;
    CMemPool::Handle framebuffers_mem[NumFramebuffers];

    dk::Image depthBuffer;
    dk::Image framebuffers[NumFramebuffers];
    DkCmdList framebuffer_cmdlists[NumFramebuffers];
    dk::UniqueSwapchain swapchain;

    DkCmdList render_cmdlist;

    std::optional<DkRenderer> renderer;
    NVGcontext* vg;

	DemoData data;

public:
    DkTest()
    {
        chooseFramebufferSize(framebufferWidth, framebufferHeight, appletGetOperationMode());

        // Create the deko3d device
        device = dk::DeviceMaker{}.setCbDebug(OutputDkDebug).create();

        // Create the main queue
        queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();

        // Create the memory pools
        pool_images.emplace(device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 16*1024*1024);
        pool_code.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, 128*1024);
        pool_data.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 1*1024*1024);

        // Create the static command buffer and feed it freshly allocated memory
        cmdbuf = dk::CmdBufMaker{device}.create();
        CMemPool::Handle cmdmem = pool_data->allocate(StaticCmdSize);
        cmdbuf.addMemory(cmdmem.getMemBlock(), cmdmem.getOffset(), cmdmem.getSize());

        // Load the shaders
        vertexShader.load(*pool_code, "romfs:/shaders/basic_vsh.dksh");
        fragmentShader.load(*pool_code, "romfs:/shaders/color_fsh.dksh");

        // Load the vertex buffer
        vertexBuffer = pool_data->allocate(sizeof(TriangleVertexData), alignof(Vertex));
        memcpy(vertexBuffer.getCpuAddr(), TriangleVertexData.data(), vertexBuffer.getSize());

        // Create the framebuffer resources
        createFramebufferResources();

        this->renderer.emplace(framebufferWidth, framebufferHeight, this->device, this->queue, *this->pool_images, *this->pool_code, *this->pool_data);
	    this->vg = nvgCreateDk(&*this->renderer, NVG_ANTIALIAS | NVG_STENCIL_STROKES);

		if (loadDemoData(vg, &this->data) == -1) {
			OutputDebugString("Failed to load demo data!\n");
		}
    }

    ~DkTest()
    {
		freeDemoData(vg, &this->data);

        // Cleanup vg. This needs to be done first as it relies on the renderer.
        nvgDeleteDk(vg);

        // Destroy the renderer
        this->renderer.reset();

        // Destroy the framebuffer resources
        destroyFramebufferResources();

        // Destroy the vertex buffer (not strictly needed in this case)
        vertexBuffer.destroy();
    }

    void createFramebufferResources()
    {
      // Create layout for the depth buffer
        dk::ImageLayout layout_depthbuffer;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
            .setFormat(DkImageFormat_S8)
            .setDimensions(framebufferWidth, framebufferHeight)
            .initialize(layout_depthbuffer);

        // Create the depth buffer
        depthBuffer_mem = pool_images->allocate(layout_depthbuffer.getSize(), layout_depthbuffer.getAlignment());
        depthBuffer.initialize(layout_depthbuffer, depthBuffer_mem.getMemBlock(), depthBuffer_mem.getOffset());

        // Create layout for the framebuffers
        dk::ImageLayout layout_framebuffer;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(framebufferWidth, framebufferHeight)
            .initialize(layout_framebuffer);

        // Create the framebuffers
        std::array<DkImage const*, NumFramebuffers> fb_array;
        uint64_t fb_size  = layout_framebuffer.getSize();
        uint32_t fb_align = layout_framebuffer.getAlignment();
        for (unsigned i = 0; i < NumFramebuffers; i ++)
        {
            // Allocate a framebuffer
            framebuffers_mem[i] = pool_images->allocate(fb_size, fb_align);
            framebuffers[i].initialize(layout_framebuffer, framebuffers_mem[i].getMemBlock(), framebuffers_mem[i].getOffset());

            // Generate a command list that binds it
            dk::ImageView colorTarget{ framebuffers[i] }, depthTarget{ depthBuffer };
            cmdbuf.bindRenderTargets(&colorTarget, &depthTarget);
            framebuffer_cmdlists[i] = cmdbuf.finishList();

            // Fill in the array for use later by the swapchain creation code
            fb_array[i] = &framebuffers[i];
        }

        // Create the swapchain using the framebuffers
        swapchain = dk::SwapchainMaker{device, nwindowGetDefault(), fb_array}.create();

        // Generate the main rendering cmdlist
        recordStaticCommands();
    }

    void destroyFramebufferResources()
    {
        // Return early if we have nothing to destroy
        if (!swapchain) return;

        // Make sure the queue is idle before destroying anything
        queue.waitIdle();

        // Clear the static cmdbuf, destroying the static cmdlists in the process
        cmdbuf.clear();

        // Destroy the swapchain
        swapchain.destroy();

        // Destroy the framebuffers
        for (unsigned i = 0; i < NumFramebuffers; i ++)
            framebuffers_mem[i].destroy();

        // Destroy the depth buffer
        depthBuffer_mem.destroy();
    }

    void recordStaticCommands()
    {
        // Initialize state structs with deko3d defaults
        dk::RasterizerState rasterizerState;
        dk::ColorState colorState;
        dk::ColorWriteState colorWriteState;
        dk::BlendState blendState;

        // Configure the viewport and scissor
        cmdbuf.setViewports(0, { { 0.0f, 0.0f, static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight), 0.0f, 1.0f } });
        cmdbuf.setScissors(0, { { 0, 0, framebufferWidth, framebufferHeight } });

        // Clear the color and depth buffers
        cmdbuf.clearColor(0, DkColorMask_RGBA, 0.2f, 0.3f, 0.3f, 1.0f);
        cmdbuf.clearDepthStencil(true, 1.0f, 0xFF, 0);

        // Bind required state
        cmdbuf.bindRasterizerState(rasterizerState);
        cmdbuf.bindColorState(colorState);
        cmdbuf.bindColorWriteState(colorWriteState);
/*
        // Bind state required for drawing the triangle
        cmdbuf.bindShaders(DkStageFlag_GraphicsMask, { vertexShader, fragmentShader });
        cmdbuf.bindRasterizerState(rasterizerState);
        cmdbuf.bindColorState(colorState);
        cmdbuf.bindColorWriteState(colorWriteState);
        cmdbuf.bindVtxBuffer(0, vertexBuffer.getGpuAddr(), vertexBuffer.getSize());
        cmdbuf.bindVtxAttribState(VertexAttribState);
        cmdbuf.bindVtxBufferState(VertexBufferState);

        // Draw the triangle
        cmdbuf.draw(DkPrimitive_Triangles, TriangleVertexData.size(), 1, 0, 0);
*/
        render_cmdlist = cmdbuf.finishList();
    }

    void render(u64 ns, bool blowup)
    {
        // Acquire a framebuffer from the swapchain (and wait for it to be available)
        int slot = queue.acquireImage(swapchain);

        // Run the command list that attaches said framebuffer to the queue
        queue.submitCommands(framebuffer_cmdlists[slot]);

        // Run the main rendering command list
        queue.submitCommands(render_cmdlist);
        
        nvgBeginFrame(vg, framebufferWidth, framebufferHeight, 1.f);
        nvgScale(vg, windowScale, windowScale);
        {
		    renderDemo(vg, 0,0, 1280, 720, static_cast<float>(ns) / 1000 / 1000 / 1000, blowup, &data);
        }
		nvgEndFrame(vg);

        // Now that we are done rendering, present it to the screen
        queue.presentImage(swapchain, slot);
    }

    void onOperationMode(AppletOperationMode mode) override {
        // Destroy the framebuffer resources
        destroyFramebufferResources();

        // Choose framebuffer size
        chooseFramebufferSize(framebufferWidth, framebufferHeight, mode);
        printf("mode changed: %dx%d\n", framebufferWidth, framebufferHeight);

        // Recreate the framebuffers and its associated resources
        createFramebufferResources();
    }

    bool onFrame(u64 ns) override
    {
        hidScanInput();
        u64 kDown = hidKeysDown(CONTROLLER_P1_AUTO);
        u64 kHeld = hidKeysHeld(CONTROLLER_P1_AUTO);
        if (kDown & KEY_PLUS)
            return false;

        bool blowup = kHeld & KEY_MINUS;

        render(ns, blowup);
        return true;
    }
};

// Main entrypoint
int main(int argc, char* argv[])
{
	printf("Nanovg Deko3D test\n");

    DkTest app;
    app.run();

    return 0;
}
#endif