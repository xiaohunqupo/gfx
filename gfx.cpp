/****************************************************************************
MIT License

Copyright (c) 2024 Guillaume Boissé

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
****************************************************************************/

#include "gfx.h"
#include "gfx_internal_types.h"

#include <map>                  // std::map
#include <deque>                // std::deque
#include <memory>               // std::unique_ptr
#include <direct.h>             // _mkdir()
#include <aclapi.h>             // SetEntriesInAcl()
#include <accctrl.h>            // EXPLICIT_ACCESS
#include <dxcapi.h>             // shader compiler
#include <d3d12shader.h>        // shader reflection
#include <dxgi1_6.h>            // IDXGIFactory6 + IDXGIOutput6
#include <filesystem>

#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wnested-anon-types"
#    pragma clang diagnostic ignored "-Wmissing-field-initializers"
#    pragma clang diagnostic ignored "-Wmisleading-indentation"
#    pragma clang diagnostic ignored "-Wswitch"
#    pragma clang diagnostic ignored "-Wunused-parameter"
#    pragma clang diagnostic ignored "-Wtautological-undefined-compare"
#    pragma clang diagnostic ignored "-Wunused-but-set-variable"
#    pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wnested-anon-types"
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#    pragma GCC diagnostic ignored "-Wmisleading-indentation"
#    pragma GCC diagnostic ignored "-Wswitch"
#    pragma GCC diagnostic ignored "-Wunused-parameter"
#    pragma GCC diagnostic ignored "-Wtautological-undefined-compare"
#    pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#    pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4100) // unreferenced formal parameter
#    pragma warning(disable : 4127) // conditional expression is constant
#    pragma warning(disable : 4189) // local variable is initialized but not referenced
#    pragma warning(disable : 4211) // nonstandard extension used: redefined extern to static
#endif
#include <D3D12MemAlloc.h>      // D3D12 memory allocator
#include <pix3.h>
#include <AmdPix3.h>
#ifdef __clang__
#    pragma clang diagnostic pop
#elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#    pragma warning(pop)
#endif

extern "C"
{
__declspec(dllexport) extern const UINT     D3D12SDKVersion = GFX_AGILITY_VERSION;
__declspec(dllexport) extern char8_t const *D3D12SDKPath = u8".\\";

__declspec(dllexport) UINT GetD3D12SDKVersion()
{
    return D3D12SDKVersion;
}
}

class GfxInternal
{
    GFX_NON_COPYABLE(GfxInternal);

    HWND window_ = {};
    uint32_t window_width_ = 0;
    uint32_t window_height_ = 0;
    uint32_t max_frames_in_flight_ = 0;

    ID3D12Device *device_ = nullptr;
    IDXGIAdapter1 *adapter_ = nullptr;
    ID3D12Device5 *dxr_device_ = nullptr;
    ID3D12Device2 *mesh_device_ = nullptr;
    ID3D12CommandQueue *command_queue_ = nullptr;
    ID3D12GraphicsCommandList *command_list_ = nullptr;
    ID3D12GraphicsCommandList4 *dxr_command_list_ = nullptr;
    ID3D12GraphicsCommandList6 *mesh_command_list_ = nullptr;
    ID3D12CommandAllocator **command_allocators_ = nullptr;
    ID3D12DebugCommandList *dbg_command_list_ = nullptr;
    std::vector<IAmdExtD3DDevice1 *> amd_ext_devices_;

    HANDLE fence_event_ = {};
    uint32_t fence_index_ = 0;
    ID3D12Fence **fences_ = nullptr;
    uint64_t *fence_values_ = nullptr;

    bool debug_shaders_ = false;
    bool cache_shaders_ = false;
    bool experimental_shaders_ = false;
    IDxcUtils *dxc_utils_ = nullptr;
    IDxcCompiler3 *dxc_compiler_ = nullptr;
    IDxcIncludeHandler *dxc_include_handler_ = nullptr;

    IDXGISwapChain3 *swap_chain_ = nullptr;
    D3D12MA::Allocator *mem_allocator_ = nullptr;
    ID3D12CommandSignature *dispatch_signature_ = nullptr;
    ID3D12CommandSignature *multi_draw_signature_ = nullptr;
    ID3D12CommandSignature *multi_draw_indexed_signature_ = nullptr;
    ID3D12CommandSignature *dispatch_rays_signature_ = nullptr;
    ID3D12CommandSignature *draw_mesh_signature_ = nullptr;
    std::vector<D3D12_RESOURCE_BARRIER> resource_barriers_;
    ID3D12Resource **back_buffers_ = nullptr;
    D3D12MA::Allocation **back_buffer_allocations_ = nullptr;
    DXGI_FORMAT back_buffer_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_COLOR_SPACE_TYPE color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    uint32_t *back_buffer_rtvs_ = nullptr;
    bool is_interop_ = false;

    GfxKernel bound_kernel_ = {};
    GfxBuffer draw_id_buffer_ = {};
    uint64_t descriptor_heap_id_ = 0;
    GfxBuffer bound_index_buffer_ = {};
    GfxBuffer bound_vertex_buffer_ = {};
    GfxBuffer texture_upload_buffer_ = {};
    GfxBuffer installed_index_buffer_ = {};
    GfxBuffer installed_vertex_buffer_ = {};
    bool force_install_index_buffer_ = false;
    bool force_install_vertex_buffer_ = false;
    bool force_install_draw_id_buffer_ = false;
    GfxBuffer raytracing_scratch_buffer_ = {};
    GfxBuffer *constant_buffer_pool_ = nullptr;
    uint64_t *constant_buffer_pool_cursors_ = nullptr;
    std::vector<GfxRaytracingPrimitive> active_raytracing_primitives_;

    struct RenderTarget
    {
        GfxTexture texture_ = {};
        uint32_t mip_level_ = 0;
        uint32_t slice_ = 0;
    };
    RenderTarget bound_color_targets_[kGfxConstant_MaxRenderTarget] = {};
    RenderTarget bound_depth_stencil_target_ = {};

    struct Viewport
    {
        inline Viewport &operator =(D3D12_VIEWPORT const &other)
        {
            x_ = other.TopLeftX;
            y_ = other.TopLeftY;
            width_ = other.Width;
            height_ = other.Height;

            return *this;
        }

        inline bool operator !=(D3D12_VIEWPORT const &other) const
        {
            return (x_ != other.TopLeftX || y_ != other.TopLeftY || width_ != other.Width || height_ != other.Height);
        }

        inline void invalidate()
        {
            width_ = NAN;
            height_ = NAN;
        }

        float x_ = 0.0f;
        float y_ = 0.0f;
        float width_ = 0.0f;
        float height_ = 0.0f;
    };
    Viewport viewport_;
    Viewport bound_viewport_;

    struct ScissorRect
    {
        inline ScissorRect &operator =(D3D12_RECT const &other)
        {
            x_ = (int32_t)other.left;
            y_ = (int32_t)other.top;
            width_ = (int32_t)(other.right - other.left);
            height_ = (int32_t)(other.bottom - other.top);

            return *this;
        }

        inline bool operator !=(D3D12_RECT const &other) const
        {
            return (x_ != other.left || y_ != other.top || x_ + width_ != other.right || y_ + height_ != other.bottom);
        }

        inline void invalidate()
        {
            width_ = -1;
            height_ = -1;
        }

        int32_t x_ = 0;
        int32_t y_ = 0;
        int32_t width_ = 0;
        int32_t height_ = 0;
    };
    ScissorRect scissor_rect_;
    ScissorRect bound_scissor_rect_;

    GfxKernel clear_buffer_kernel_ = {};
    GfxProgram clear_buffer_program_ = {};
    bool issued_clear_buffer_warning_ = false;

    GfxKernel copy_to_backbuffer_kernel_ = {};
    GfxProgram copy_to_backbuffer_program_ = {};

    struct MipKernels
    {
        GfxProgram mip_program_ = {};
        GfxKernel mip_kernel_ = {};
    };
    std::map<uint32_t, MipKernels> mip_kernels_;

    struct ScanKernels
    {
        GfxProgram scan_program_ = {};
        GfxKernel reduce_kernel_ = {};
        GfxKernel scan_add_kernel_ = {};
        GfxKernel scan_kernel_ = {};
        GfxKernel args_kernel_ = {};
    };
    std::map<uint32_t, ScanKernels> scan_kernels_;

    struct SortKernels
    {
        GfxProgram sort_program_ = {};
        GfxKernel histogram_kernel_ = {};
        GfxKernel scatter_kernel_ = {};
        GfxKernel args_kernel_ = {};
    };
    GfxBuffer sort_scratch_buffer_;
    std::map<uint32_t, SortKernels> sort_kernels_;

    struct String
    {
        char *data_;
        GFX_NON_COPYABLE(String);
        inline String() : data_(nullptr) {}
        inline String(char const *data) : data_(nullptr) { *this = data; }
        inline String(String &&other) : data_(other.data_) { other.data_ = nullptr; }
        inline String &operator =(String &&other) { if(this != &other) { gfxFree(data_); data_ = other.data_; other.data_ = nullptr; } return *this; }
        inline String &operator =(char const *data) { gfxFree(data_); if(!data) data_ = nullptr; else
                                                                           { data_ = (char *)gfxMalloc(strlen(data) + 1); strcpy(data_, data); }
                                                      return *this; }
        inline operator char const *() const { return data_ ? data_ : ""; }
        inline size_t size() const { return data_ ? strlen(data_) : 0; }
        inline char const *c_str() const { return data_ ? data_ : ""; }
        inline operator bool() const { return data_ != nullptr; }
        inline ~String() { gfxFree(data_); }
    };

    struct Garbage
    {
        GFX_NON_COPYABLE(Garbage);
        typedef void (*Collector)(Garbage const &garbage);
        inline Garbage() : deletion_counter_(0), garbage_collector_(nullptr) { memset(garbage_, 0, sizeof(garbage_)); }
        inline Garbage(Garbage &&other) : deletion_counter_(other.deletion_counter_), garbage_collector_(other.garbage_collector_) { memcpy(garbage_, other.garbage_, sizeof(garbage_)); other.garbage_collector_ = nullptr; }
        inline Garbage &operator =(Garbage &&other) { if(this != &other) { GFX_ASSERT(!garbage_collector_); if(garbage_collector_) garbage_collector_(*this); memcpy(garbage_, other.garbage_, sizeof(garbage_)); deletion_counter_ = other.deletion_counter_; garbage_collector_ = other.garbage_collector_; other.garbage_collector_ = nullptr; } return *this; }
        inline ~Garbage() { GFX_ASSERT(!!deletion_counter_ || !garbage_collector_); if(garbage_collector_) garbage_collector_(*this); }

        template<typename TYPE>
        static void ResourceCollector(Garbage const &garbage)
        {
            TYPE *resource = (TYPE *)garbage.garbage_[0];
            GFX_ASSERT(resource != nullptr); if(resource == nullptr) return;
            resource->Release();    // release resource
        }

        static void DescriptorCollector(Garbage const &garbage)
        {
            uint32_t const descriptor_slot = (uint32_t)garbage.garbage_[0];
            GfxFreelist *descriptor_freelist = (GfxFreelist *)garbage.garbage_[1];
            GFX_ASSERT(descriptor_freelist != nullptr); if(descriptor_freelist == nullptr) return;
            descriptor_freelist->free_slot(descriptor_slot);    // release descriptor slot
        }

        uintptr_t garbage_[2];
        uint32_t deletion_counter_;
        Collector garbage_collector_;
    };
    std::deque<Garbage> garbage_collection_;

    struct DescriptorHeap
    {
        inline D3D12_CPU_DESCRIPTOR_HANDLE getCPUHandle(uint32_t descriptor_slot) const
        {
            GFX_ASSERT(descriptor_slot < (descriptor_heap_ != nullptr ? descriptor_heap_->GetDesc().NumDescriptors : 0));
            D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
            cpu_handle.ptr += descriptor_slot * descriptor_handle_size_;
            return cpu_handle;
        }

        inline D3D12_GPU_DESCRIPTOR_HANDLE getGPUHandle(uint32_t descriptor_slot) const
        {
            GFX_ASSERT(descriptor_slot < (descriptor_heap_ != nullptr ? descriptor_heap_->GetDesc().NumDescriptors : 0));
            D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
            gpu_handle.ptr += descriptor_slot * descriptor_handle_size_;
            return gpu_handle;
        }

        uint32_t descriptor_handle_size_ = 0;
        ID3D12DescriptorHeap *descriptor_heap_ = nullptr;
    };
    DescriptorHeap descriptors_;
    DescriptorHeap dsv_descriptors_;
    DescriptorHeap rtv_descriptors_;
    DescriptorHeap sampler_descriptors_;
    GfxFreelist freelist_descriptors_;
    GfxFreelist freelist_dsv_descriptors_;
    GfxFreelist freelist_rtv_descriptors_;
    GfxFreelist freelist_sampler_descriptors_;

    struct DrawState
    {
        struct Data
        {
            DXGI_FORMAT                   color_formats_[kGfxConstant_MaxRenderTarget] = {};
            DXGI_FORMAT                   depth_stencil_format_                        = {};
            D3D12_PRIMITIVE_TOPOLOGY_TYPE primitive_topology_type_                     = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            struct
            {
                inline operator bool() const
                {
                    return (src_blend_ != 0 && dst_blend_ != 0 && blend_op_ != 0 && src_blend_alpha_ != 0 && dst_blend_alpha_ != 0 && blend_op_alpha_ != 0);
                }

                D3D12_BLEND src_blend_ = {};
                D3D12_BLEND dst_blend_ = {};
                D3D12_BLEND_OP blend_op_ = {};
                D3D12_BLEND src_blend_alpha_ = {};
                D3D12_BLEND dst_blend_alpha_ = {};
                D3D12_BLEND_OP blend_op_alpha_ = {};
            } blend_state_;
            struct
            {
                D3D12_COMPARISON_FUNC  depth_func_       = D3D12_COMPARISON_FUNC_LESS;
                D3D12_DEPTH_WRITE_MASK depth_write_mask_ = D3D12_DEPTH_WRITE_MASK_ALL;
            } depth_stencil_state_;
            struct
            {
                D3D12_CULL_MODE cull_mode_ = D3D12_CULL_MODE_BACK;
                D3D12_FILL_MODE fill_mode_ = D3D12_FILL_MODE_SOLID;
            } raster_state_;
        };

        DrawState() : reference_count_(0) {}
        DrawState(DrawState &&other) : draw_state_(other.draw_state_), reference_count_(other.reference_count_) { other.reference_count_ = 0; }
        ~DrawState() { GFX_ASSERT(reference_count_ == 0); }

        inline DrawState &operator =(DrawState &&other)
        {
            if(this != &other)
            {
                GFX_ASSERT(reference_count_ == 0);
                draw_state_ = other.draw_state_;
                reference_count_ = other.reference_count_;
                other.reference_count_ = 0;
            }
            return *this;
        }

        Data draw_state_;
        uint32_t reference_count_;
    };
    static GfxArray<DrawState> draw_states_;
    static GfxHandles draw_state_handles_;

    struct Object
    {
        enum Flag
        {
            kFlag_Named = 1 << 0
        };
        uint32_t flags_ = 0;
    };

    struct Buffer : public Object
    {
        inline bool isInterop() const { return (allocation_ == nullptr ? true : false); }

        void *data_ = nullptr;
        uint64_t data_offset_ = 0;
        bool *transitioned_ = nullptr;
        ID3D12Resource *resource_ = nullptr;
        uint32_t *reference_count_ = nullptr;
        D3D12MA::Allocation *allocation_ = nullptr;
        D3D12_RESOURCE_STATES *resource_state_ = nullptr;
        D3D12_RESOURCE_STATES initial_resource_state_ = D3D12_RESOURCE_STATE_COMMON;
    };
    GfxArray<Buffer> buffers_;
    GfxHandles buffer_handles_;

    struct Texture : public Object
    {
        enum Flag
        {
            kFlag_AutoResize = 1 << 0
        };

        inline bool isInterop() const { return (allocation_ == nullptr ? true : false); }

        uint32_t flags_ = 0;
        bool transitioned_ = false;
        float clear_value_[4] = {};
        ID3D12Resource *resource_ = nullptr;
        D3D12MA::Allocation *allocation_ = nullptr;
        std::vector<uint32_t> dsv_descriptor_slots_[D3D12_REQ_MIP_LEVELS];
        std::vector<uint32_t> rtv_descriptor_slots_[D3D12_REQ_MIP_LEVELS];
        D3D12_RESOURCE_STATES resource_state_ = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES initial_resource_state_ = D3D12_RESOURCE_STATE_COMMON;
    };
    GfxArray<Texture> textures_;
    GfxHandles texture_handles_;

    struct SamplerState
    {
        D3D12_SAMPLER_DESC sampler_desc_ = {};
        uint32_t descriptor_slot_ = 0xFFFFFFFFu;
    };
    GfxArray<SamplerState> sampler_states_;
    GfxHandles sampler_state_handles_;

    struct AccelerationStructure
    {
        bool needs_update_ = false;
        bool needs_rebuild_ = false;
        GfxBuffer bvh_buffer_ = {};
        uint64_t bvh_data_size_ = 0;
        std::vector<GfxRaytracingPrimitive> raytracing_primitives_;
    };
    GfxArray<AccelerationStructure> acceleration_structures_;
    GfxHandles acceleration_structure_handles_;

    struct RaytracingPrimitive
    {
        uint32_t index_ = 0;
        float transform_[16] = {};
        uint32_t instance_id_ = 0;
        uint8_t instance_mask_ = 0xFFu;
        uint32_t instance_contribution_to_hit_group_index_ = 0;
        enum
        {
            kType_Triangles = 0,
            kType_Instance,
            kType_Procedural,

            kType_Count
        }
        type_;
        struct
        {
            uint32_t build_flags_ = 0;
            GfxBuffer bvh_buffer_ = {};
            uint64_t bvh_data_size_ = 0;
            uint32_t index_stride_ = 0;
            GfxBuffer index_buffer_ = {};
            uint32_t vertex_stride_ = 0;
            GfxBuffer vertex_buffer_ = {};
            GfxAccelerationStructure acceleration_structure_ = {};
        }
        triangles_;
        struct
        {
            GfxRaytracingPrimitive parent_ = {};
        }
        instance_;
        struct
        {
            uint32_t build_flags_ = 0;
            GfxBuffer bvh_buffer_ = {};
            uint64_t bvh_data_size_ = 0;
            uint32_t procedural_stride_ = 0;
            GfxBuffer procedural_buffer_ = {};
            GfxAccelerationStructure acceleration_structure_ = {};
        }
        procedural_;
    };
    GfxArray<RaytracingPrimitive> raytracing_primitives_;
    GfxHandles raytracing_primitive_handles_;

    struct Program
    {
        struct Parameter
        {
            enum Type
            {
                kType_Buffer = 0,
                kType_Image,
                kType_SamplerState,
                kType_AccelerationStructure,
                kType_Constants,

                kType_Count
            };

            String name_;
            uint32_t id_ = 0;
            Type type_ = kType_Count;
            union
            {
                struct { GfxBuffer *buffers_; uint32_t buffer_count; } buffer_;
                struct { GfxTexture *textures_; uint32_t *mip_levels_; uint32_t texture_count; } image_;
                GfxSamplerState sampler_state_;
                struct { GfxAccelerationStructure bvh_; GfxBuffer bvh_buffer_; } acceleration_structure_;
                void *constants_;
            }
            data_ = {};
            uint32_t data_size_ = 0;

            void set(GfxBuffer const *buffers, uint32_t buffer_count)
            {
                GFX_ASSERT(buffers != nullptr || buffer_count == 0);
                if(type_ == kType_Buffer && data_.buffer_.buffer_count == buffer_count)
                    for(uint32_t i = 0; i < buffer_count; ++i) { if(buffers[i].handle != data_.buffer_.buffers_[i].handle) { ++id_; break; } }
                else
                {
                    unset();
                    type_ = kType_Buffer;
                    data_.buffer_.buffer_count = buffer_count;
                    data_size_ = buffer_count * sizeof(GfxBuffer);
                    data_.buffer_.buffers_ = (GfxBuffer *)(buffer_count > 0 ? gfxMalloc(buffer_count * sizeof(GfxBuffer)) : nullptr);
                }
                for(uint32_t i = 0; i < buffer_count; ++i)
                    data_.buffer_.buffers_[i] = buffers[i];
            }

            void set(GfxTexture const *textures, uint32_t const *mip_levels, uint32_t texture_count)
            {
                GFX_ASSERT(textures != nullptr || texture_count == 0);
                if(type_ == kType_Image && data_.image_.texture_count == texture_count)
                    for(uint32_t i = 0; i < texture_count; ++i) { if(textures[i].handle != data_.image_.textures_[i].handle ||
                                                                    (data_.image_.mip_levels_ != nullptr ? data_.image_.mip_levels_[i] : 0) != (mip_levels != nullptr ? mip_levels[i] : 0)) { ++id_; break; } }
                else
                {
                    unset();
                    type_ = kType_Image;
                    data_.image_.texture_count = texture_count;
                    data_size_ = texture_count * sizeof(GfxTexture);
                    data_.image_.textures_ = (GfxTexture *)(texture_count > 0 ? gfxMalloc(texture_count * sizeof(GfxTexture)) : nullptr);
                    data_.image_.mip_levels_ = (uint32_t *)(texture_count > 0 && mip_levels != nullptr ? gfxMalloc(texture_count * sizeof(uint32_t)) : nullptr);
                }
                for(uint32_t i = 0; i < texture_count; ++i)
                {
                    data_.image_.textures_[i] = textures[i];
                    if(mip_levels != nullptr)
                        data_.image_.mip_levels_[i] = mip_levels[i];
                }
            }

            void set(GfxSamplerState const &sampler_state)
            {
                if(type_ == kType_SamplerState)
                    id_ += (sampler_state.handle != data_.sampler_state_.handle);
                else
                {
                    unset();
                    type_ = kType_SamplerState;
                    data_size_ = sizeof(sampler_state);
                }
                data_.sampler_state_ = sampler_state;
            }

            void set(GfxAccelerationStructure const &acceleration_structure)
            {
                if(type_ == kType_AccelerationStructure)
                    id_ += (acceleration_structure.handle != data_.acceleration_structure_.bvh_.handle);
                else
                {
                    unset();
                    type_ = kType_AccelerationStructure;
                    data_size_ = sizeof(acceleration_structure);
                }
                data_.acceleration_structure_.bvh_ = acceleration_structure;
            }

            void set(void const *data, uint32_t data_size)
            {
                if(type_ == kType_Constants && data_size_ == data_size)
                    id_ += (memcmp(data, data_.constants_, data_size) != 0);
                else
                {
                    unset();
                    type_ = kType_Constants;
                    data_.constants_ = (data_size > 0 ? gfxMalloc(data_size) : nullptr);
                }
                memcpy(data_.constants_, data, data_size);
                data_size_ = data_size;
            }

            void unset()
            {
                switch(type_)
                {
                case kType_Image:
                    gfxFree(data_.image_.textures_);
                    gfxFree(data_.image_.mip_levels_);
                    break;
                case kType_Constants:
                    gfxFree(data_.constants_);
                    break;
                default:
                    break;
                }
                memset(&data_, 0, sizeof(data_));
                type_ = kType_Count;
                data_size_ = 0;
                ++id_;
            }

            char const *getTypeName() const
            {
                switch(type_)
                {
                case kType_Buffer:
                    return "Buffer";
                case kType_Image:
                    return "Texture";
                case kType_SamplerState:
                    return "Sampler state";
                case kType_AccelerationStructure:
                    return "Acceleration structure";
                case kType_Constants:
                    return "Constants";
                default:
                    break;  // undefined type
                }
                return "undefined";
            }
        };

        typedef std::map<uint64_t, Parameter> Parameters;

        Parameter &insertParameter(char const *parameter_name)
        {
            uint64_t const parameter_id = Hash(parameter_name);
            Parameters::iterator const it = parameters_.find(parameter_id);
            GFX_ASSERT(parameter_name != nullptr && *parameter_name != '\0');
            if(it == parameters_.end())
            {
                Parameter &parameter = parameters_[parameter_id];
                parameter.name_ = parameter_name;
                return parameter;
            }
            GFX_ASSERT(strcmp((*it).second.name_.c_str(), parameter_name) == 0);
            return (*it).second;    // ^ assert on hashing conflicts
        }

        String cs_;
        String as_;
        String ms_;
        String vs_;
        String gs_;
        String ps_;
        String lib_;
        String file_name_;
        String file_path_;
        String shader_model_;
        Parameters parameters_;
        std::vector<String> include_paths_;
    };
    GfxArray<Program> programs_;
    GfxHandles program_handles_;

    struct Kernel
    {
        enum Type
        {
            kType_Mesh = 0,
            kType_Compute,
            kType_Graphics,
            kType_Raytracing,

            kType_Count
        };

        inline bool isMesh() const { return type_ == kType_Mesh; }
        inline bool isCompute() const { return type_ == kType_Compute; }
        inline bool isGraphics() const { return type_ == kType_Graphics; }
        inline bool isRaytracing() const { return type_ == kType_Raytracing; }

        struct Parameter
        {
            enum Type
            {
                kType_Buffer = 0,
                kType_RWBuffer,

                kType_Texture2D,
                kType_RWTexture2D,

                kType_Texture2DArray,
                kType_RWTexture2DArray,

                kType_Texture3D,
                kType_RWTexture3D,

                kType_TextureCube,

                kType_AccelerationStructure,

                kType_Constants,
                kType_ConstantBuffer,

                kType_Sampler,

                kType_Count
            };

            Type type_ = kType_Count;
            uint32_t id_ = 0xFFFFFFFFu;
            uint64_t parameter_id_ = 0;
            uint32_t descriptor_count_ = 0;
            uint32_t descriptor_slot_ = 0xFFFFFFFFu;
            std::vector<ID3D12Resource *> bound_textures_;
            Program::Parameter const *parameter_ = nullptr;
            bool raw_access_ = false;

            struct Variable
            {
                uint32_t id_ = 0;
                uint32_t data_size_ = 0;
                uint32_t data_start_ = 0;
                uint64_t parameter_id_ = 0;
                Program::Parameter const *parameter_ = nullptr;
            };

            Variable *variables_ = nullptr;
            uint32_t variable_count_ = 0;
            uint32_t variable_size_ = 0;
        };

        struct LocalParameter
        {
            ID3D12RootSignature *local_root_signature_ = nullptr;
            std::vector<Parameter> parameters_;
        };

        struct LocalRootSignatureAssociation
        {
            uint32_t local_root_signature_space = 0;
            GfxShaderGroupType shader_group_type = kGfxShaderGroupType_Count;
        };

        String entry_point_;
        GfxProgram program_ = {};
        Type type_ = kType_Count;
        DrawState::Data draw_state_;
        std::vector<String> defines_;
        std::vector<String> exports_;
        std::vector<String> subobjects_;
        std::map<std::wstring, LocalRootSignatureAssociation> local_root_signature_associations_;
        uint64_t descriptor_heap_id_ = 0;
        uint32_t *num_threads_ = nullptr;
        IDxcBlob *cs_bytecode_ = nullptr;
        IDxcBlob *as_bytecode_ = nullptr;
        IDxcBlob *ms_bytecode_ = nullptr;
        IDxcBlob *vs_bytecode_ = nullptr;
        IDxcBlob *gs_bytecode_ = nullptr;
        IDxcBlob *ps_bytecode_ = nullptr;
        IDxcBlob *lib_bytecode_ = nullptr;
        ID3D12ShaderReflection *cs_reflection_ = nullptr;
        ID3D12ShaderReflection *as_reflection_ = nullptr;
        ID3D12ShaderReflection *ms_reflection_ = nullptr;
        ID3D12ShaderReflection *vs_reflection_ = nullptr;
        ID3D12ShaderReflection *gs_reflection_ = nullptr;
        ID3D12ShaderReflection *ps_reflection_ = nullptr;
        ID3D12LibraryReflection *lib_reflection_ = nullptr;
        ID3D12RootSignature *root_signature_ = nullptr;
        std::map<uint32_t, LocalParameter> local_parameters_;
        size_t sbt_record_stride_[kGfxShaderGroupType_Count] = {};
        ID3D12PipelineState *pipeline_state_ = nullptr;
        ID3D12StateObject *state_object_ = nullptr;
        Parameter *parameters_ = nullptr;
        uint32_t parameter_count_ = 0;
        uint32_t vertex_stride_ = 0;
    };
    GfxArray<Kernel> kernels_;
    GfxHandles kernel_handles_;

    enum ShaderType
    {
        kShaderType_CS = 0,
        kShaderType_AS,
        kShaderType_MS,
        kShaderType_VS,
        kShaderType_GS,
        kShaderType_PS,
        kShaderType_LIB,

        kShaderType_Count
    };
    static char const *shader_extensions_[kShaderType_Count];
    uint32_t dummy_descriptors_[Kernel::Parameter::kType_Count] = {};
    uint32_t dummy_rtv_descriptor_ = 0xFFFFFFFFu;

    struct TimestampQuery
    {
        float duration_ = 0.0f;
        bool was_begun_ = false;
    };
    GfxArray<TimestampQuery> timestamp_queries_;
    GfxHandles timestamp_query_handles_;

    struct TimestampQueryHeap
    {
        GfxBuffer query_buffer_ = {};
        ID3D12QueryHeap *query_heap_ = nullptr;
        std::map<uint64_t, std::pair<uint32_t, GfxTimestampQuery>> timestamp_queries_;
    };
    uint64_t timestamp_query_ticks_per_second_ = 0;
    TimestampQueryHeap *timestamp_query_heaps_ = nullptr;

    struct Sbt
    {
        struct ShaderRecord
        {
            uint32_t id_ = 0;
            uint32_t commited_id_ = 0xFFFFFFFFu;
            std::wstring shader_identifier_;
            std::vector<Kernel::Parameter> bound_parameters_;
            std::unique_ptr<Program::Parameters> parameters_;
        };

        ShaderRecord &insertSbtRecord(GfxShaderGroupType shader_group_type, uint32_t index)
        {
            auto const it = shader_records_[shader_group_type].find(index);
            if(it == shader_records_[shader_group_type].end())
            {
                ShaderRecord &record = shader_records_[shader_group_type][index];
                record.parameters_ = std::make_unique<Program::Parameters>();
                return record;
            }
            return (*it).second;
        }

        void insertSbtRecordShaderIdentifier(GfxShaderGroupType shader_group_type, uint32_t index, WCHAR *shader_identifier)
        {
            ShaderRecord &record = insertSbtRecord(shader_group_type, index);
            record.id_ += record.shader_identifier_ != shader_identifier;
            record.shader_identifier_ = shader_identifier;
        }

        std::pair<ShaderRecord &, Program::Parameter &> insertSbtRecordParameter(GfxShaderGroupType shader_group_type, uint32_t index, char const *parameter_name)
        {
            uint64_t const parameter_id = Hash(parameter_name);
            ShaderRecord &record = insertSbtRecord(shader_group_type, index);
            Program::Parameters::iterator const it = record.parameters_->find(parameter_id);
            GFX_ASSERT(parameter_name != nullptr && *parameter_name != '\0');
            if(it == record.parameters_->end())
            {
                Program::Parameter &parameter = (*record.parameters_)[parameter_id];
                parameter.name_ = parameter_name;
                return {record, parameter};
            }
            GFX_ASSERT(strcmp((*it).second.name_.c_str(), parameter_name) == 0);
            return {record, (*it).second};  // ^ assert on hashing conflicts
        }

        std::map<uint32_t, ShaderRecord> shader_records_[kGfxShaderGroupType_Count];
        uint64_t descriptor_heap_id_ = 0;
        GfxBuffer sbt_buffers_[kGfxShaderGroupType_Count] = {};
        size_t sbt_max_record_stride_[kGfxShaderGroupType_Count];
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE ray_generation_shader_record_;
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE miss_shader_table_;
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE hit_group_table_;
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE callable_shader_table_;
        GfxKernel kernel_ = {};
    };
    GfxArray<Sbt> sbts_;
    GfxHandles sbt_handles_;

    struct WindowsSecurityAttributes
    {
        SECURITY_ATTRIBUTES security_attributes_ = {};
        PSECURITY_DESCRIPTOR security_descriptor_ = {};

        inline SECURITY_ATTRIBUTES *operator &()
        {
            return &security_attributes_;
        }

        inline WindowsSecurityAttributes()
        {
            security_descriptor_ = (PSECURITY_DESCRIPTOR)gfxMalloc(SECURITY_DESCRIPTOR_MIN_LENGTH + 2 * sizeof(void**));
            GFX_ASSERT(security_descriptor_ != nullptr);
            PSID *ppSID = (PSID *)((PBYTE)security_descriptor_ + SECURITY_DESCRIPTOR_MIN_LENGTH);
            PACL *ppACL = (PACL *)((PBYTE)ppSID + sizeof(PSID *));
            InitializeSecurityDescriptor(security_descriptor_, SECURITY_DESCRIPTOR_REVISION);
            SID_IDENTIFIER_AUTHORITY identifier_authority = SECURITY_WORLD_SID_AUTHORITY;
            AllocateAndInitializeSid(&identifier_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, ppSID);
            EXPLICIT_ACCESS
            explicit_access                      = {};
            explicit_access.grfAccessPermissions = STANDARD_RIGHTS_ALL | SPECIFIC_RIGHTS_ALL;
            explicit_access.grfAccessMode        = SET_ACCESS;
            explicit_access.grfInheritance       = INHERIT_ONLY;
            explicit_access.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
            explicit_access.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
            explicit_access.Trustee.ptstrName    = (LPTSTR)*ppSID;
            SetEntriesInAcl(1, &explicit_access, nullptr, ppACL);
            SetSecurityDescriptorDacl(security_descriptor_, TRUE, *ppACL, FALSE);
            security_attributes_.nLength              = sizeof(security_attributes_);
            security_attributes_.lpSecurityDescriptor = security_descriptor_;
            security_attributes_.bInheritHandle       = TRUE;
        }

        inline ~WindowsSecurityAttributes()
        {
            PSID *ppSID = (PSID *)((PBYTE)security_descriptor_ + SECURITY_DESCRIPTOR_MIN_LENGTH);
            PACL *ppACL = (PACL *)((PBYTE)ppSID + sizeof(PSID *));
            FreeSid(*ppSID);
            LocalFree(*ppACL);
            gfxFree(security_descriptor_);
        }
    };

public:
    GfxInternal(GfxContext &gfx) : buffer_handles_("buffer"), texture_handles_("texture"), sampler_state_handles_("sampler state")
                                 , acceleration_structure_handles_("acceleration structure"), raytracing_primitive_handles_("raytracing primitive")
                                 , program_handles_("program"), kernel_handles_("kernel"), timestamp_query_handles_("timestamp query"), sbt_handles_("shader binding table")
                                 { gfx.handle = reinterpret_cast<uint64_t>(this); }
    ~GfxInternal() { terminate(); }

    GfxResult initialize(HWND window, GfxCreateContextFlags flags, IDXGIAdapter *adapter, GfxContext &context)
    {
        if(!window)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "An invalid window handle was supplied");
        IDXGIFactory4 *factory = nullptr;
        if(!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create DXGI factory");

        if((flags & kGfxCreateContextFlag_EnableExperimentalShaders) != 0)
        {
            IID const features[] = { D3D12ExperimentalShaderModels };
            if(!IsDeveloperModeEnabled())
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to enable experimental shaders without Windows developer mode");
            if(!SUCCEEDED(D3D12EnableExperimentalFeatures(ARRAYSIZE(features), features, nullptr, nullptr)))
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to enable experimental shaders");
            experimental_shaders_ = true;
        }

        struct DXGIFactoryReleaser
        {
            IDXGIFactory4 *factory;
            GFX_NON_COPYABLE(DXGIFactoryReleaser);
            DXGIFactoryReleaser(IDXGIFactory4 *factory) : factory(factory) {}
            ~DXGIFactoryReleaser() { factory->Release(); }
        };
        DXGIFactoryReleaser const factory_releaser(factory);
        GFX_TRY(initializeDevice(flags, adapter, factory));

        window_ = window;
        RECT window_rect = {};
        GetClientRect(window_, &window_rect);
        window_width_ = window_rect.right - window_rect.left;
        window_height_ = window_rect.bottom - window_rect.top;

        IDXGIOutput *output = nullptr;
        color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        UINT output_i  = 0;
        LONG best_area = -1;
        IDXGIOutput *current_output;
        while(adapter_->EnumOutputs(output_i, &current_output) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_OUTPUT_DESC output_desc;
            if(SUCCEEDED(current_output->GetDesc(&output_desc))) {
                RECT rect = output_desc.DesktopCoordinates;
                int intersect_area =
                    GFX_MAX(0L, GFX_MIN(window_rect.right, rect.right) - GFX_MAX(window_rect.left, rect.left)) *
                    GFX_MAX(0l, GFX_MIN(window_rect.bottom, rect.bottom) - GFX_MAX(window_rect.top, rect.top));
                if(intersect_area > best_area)
                {
                    output    = current_output;
                    best_area = intersect_area;
                }
            }
            if(current_output != output) current_output->Release();
            output_i++;
        }
        back_buffer_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
        if(output != nullptr)
        {
            IDXGIOutput6 *output6 = nullptr;
            output->QueryInterface(&output6);
            if(output6 != nullptr)
            {
                DXGI_OUTPUT_DESC1 output_desc = {};
                output6->GetDesc1(&output_desc);
                if(output_desc.BitsPerColor > 8)
                    back_buffer_format_ = DXGI_FORMAT_R10G10B10A2_UNORM;
                if((flags & kGfxCreateContextFlag_EnableHDRSwapChain) != 0)
                {
                    if(output_desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                    {
                        if((flags & kGfxCreateContextFlag_EnableLinearSwapChain) == 0)
                        {
                            color_space_        = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                            back_buffer_format_ = DXGI_FORMAT_R10G10B10A2_UNORM;
                        }
                        else
                        {
                            color_space_        = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
                            back_buffer_format_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        }
                    }
                }
                output6->Release();
            }
            output->Release();
        }

        DXGI_SWAP_CHAIN_DESC1
        swap_chain_desc                  = {};
        swap_chain_desc.Width            = window_width_;
        swap_chain_desc.Height           = window_height_;
        swap_chain_desc.Format           = back_buffer_format_;
        swap_chain_desc.BufferCount      = max_frames_in_flight_;
        swap_chain_desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_chain_desc.SampleDesc.Count = 1;
        swap_chain_desc.Flags            = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        IDXGISwapChain1 *swap_chain = nullptr;
        if(!SUCCEEDED(factory->CreateSwapChainForHwnd(command_queue_, window_, &swap_chain_desc, nullptr, nullptr, &swap_chain)))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create swap chain");
        swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain_)); swap_chain->Release();
        if(!swap_chain_ || !SUCCEEDED(factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER)))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to initialize swap chain");
        fence_index_ = swap_chain_->GetCurrentBackBufferIndex();
        swap_chain_->SetColorSpace1(color_space_);

        back_buffers_ = (ID3D12Resource **)gfxMalloc(max_frames_in_flight_ * sizeof(ID3D12Resource *));
        GFX_TRY(acquireSwapChainBuffers());
        back_buffer_rtvs_ = (uint32_t *)gfxMalloc(max_frames_in_flight_ * sizeof(uint32_t));
        GFX_TRY(createBackBufferRTVs());

        D3D12_RESOURCE_BARRIER resource_barrier = {};
        resource_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resource_barrier.Transition.pResource = back_buffers_[fence_index_];
        resource_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        resource_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        resource_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &resource_barrier);

        return initializeCommon(context);
    }

    GfxResult initialize(uint32_t width, uint32_t height, GfxCreateContextFlags flags, IDXGIAdapter *adapter, GfxContext &context)
    {
        IDXGIFactory1 *factory = nullptr;
        if(!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create DXGI factory");

        struct DXGIFactoryReleaser
        {
            IDXGIFactory1 *factory;
            GFX_NON_COPYABLE(DXGIFactoryReleaser);
            DXGIFactoryReleaser(IDXGIFactory1 *factory) : factory(factory) {}
            ~DXGIFactoryReleaser() { factory->Release(); }
        };
        DXGIFactoryReleaser const factory_releaser(factory);
        GFX_TRY(initializeDevice(flags, adapter, factory));

        window_ = nullptr;
        window_width_ = width;
        window_height_ = height;
        fence_index_ = 0;

        GFX_TRY(initializeCommon(context));

        back_buffers_ = (ID3D12Resource **)gfxMalloc(max_frames_in_flight_ * sizeof(ID3D12Resource *));
        back_buffer_allocations_ = (D3D12MA::Allocation **)gfxMalloc(max_frames_in_flight_ * sizeof(D3D12MA::Allocation *));
        GFX_TRY(createBackBuffers());
        back_buffer_rtvs_ = (uint32_t *)gfxMalloc(max_frames_in_flight_ * sizeof(uint32_t));
        GFX_TRY(createBackBufferRTVs());
        for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
            command_list_->DiscardResource(back_buffers_[i], nullptr);

        return kGfxResult_NoError;
    }

    GfxResult initializeDevice(GfxCreateContextFlags flags, IDXGIAdapter *adapter, IDXGIFactory1 *factory)
    {
        if(GetD3D12SDKVersion() != GFX_AGILITY_VERSION)
            return GFX_SET_ERROR(kGfxResult_InternalError, "Agility SDK version not exported correctly");
        if((flags & kGfxCreateContextFlag_EnableDebugLayer) != 0)
        {
            ID3D12Debug1 *debug_controller = nullptr;
            if(!SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller))))
                GFX_PRINTLN("Warning: Unable to get D3D12 debug interface, no debugging information will be available");
            else
            {
                debug_controller->EnableDebugLayer();
                //debug_controller->SetEnableGPUBasedValidation(true);
                debug_controller->SetEnableSynchronizedCommandQueueValidation(true);
                debug_controller->Release();
            }
        }

        if(adapter != nullptr)
        {
            DXGI_ADAPTER_DESC desc = {}; adapter->GetDesc(&desc);
            IDXGIFactory4 *factory4;
            if(!SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory4))))
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create DXGIFactory4");
            if(!SUCCEEDED(factory4->EnumAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&adapter_))))
                return GFX_SET_ERROR(kGfxResult_InternalError, "An invalid adapter was supplied");
            if(!SUCCEEDED(D3D12CreateDevice(adapter_, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device_))))
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create D3D12 device");
            if((flags & kGfxCreateContextFlag_EnableStablePowerState) != 0 && IsDeveloperModeEnabled() && !SUCCEEDED(device_->SetStablePowerState(TRUE)))
            {
                GFX_PRINT_ERROR(kGfxResult_InternalError, "Unable to set stable power state, is developer mode enabled?");
                device_->Release(); device_ = nullptr;  // release crashed device and try to re-create it
                if(!SUCCEEDED(D3D12CreateDevice(adapter_, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device_))))
                    return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create D3D12 device");
            }
        }
        else
        {
            IDXGIFactory6 *factory6;
            IDXGIAdapter1 *adapters[8] = {};
            if(SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6))))
            {
                for(uint32_t i = 0; i < ARRAYSIZE(adapters); ++i)
                {
                    IDXGIAdapter1 *adapter1 = nullptr;
                    if(!SUCCEEDED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter1))))
                        break;
                    adapters[i] = adapter1;
                }
            }
            else
            {
                DXGI_ADAPTER_DESC1 adapter_desc = {};
                uint32_t adapter_scores[ARRAYSIZE(adapters)] = {};
                DXGI_ADAPTER_DESC1 adapter_descs[ARRAYSIZE(adapters)] = {};
                for(uint32_t i = 0; i < ARRAYSIZE(adapters); ++i)
                {
                    IDXGIAdapter1 *adapter1 = nullptr;
                    if(!SUCCEEDED(factory->EnumAdapters1(i, &adapter1)))
                        break;
                    uint32_t j, adapter_score;
                    adapter1->GetDesc1(&adapter_desc);
                    if((adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                        adapter_score = 0;
                    else
                        switch(adapter_desc.VendorId)
                        {
                        case 0x1002u:   // AMD
                            adapter_score = 2;
                            break;
                        case 0x10DEu:   // NVIDIA
                            adapter_score = 2;
                            break;
                        default:
                            adapter_score = 1;
                            break;
                        }
                    for(j = 0; j < i; ++j)
                        if(adapter_score > adapter_scores[j] ||
                           adapter_desc.DedicatedVideoMemory > adapter_descs[j].DedicatedVideoMemory ||
                           adapter_desc.SharedSystemMemory > adapter_descs[j].SharedSystemMemory)
                            break;
                    for(uint32_t k = i; k > j; --k)
                    {
                        adapters[k] = adapters[k - 1];
                        adapter_descs[k] = adapter_descs[k - 1];
                        adapter_scores[k] = adapter_scores[k - 1];
                    }
                    adapters[j] = adapter1;
                    adapter_descs[j] = adapter_desc;
                    adapter_scores[j] = adapter_score;
                }
            }

            struct DXGIAdapterReleaser
            {
                GFX_NON_COPYABLE(DXGIAdapterReleaser);
                IDXGIAdapter1 *(&adapters)[ARRAYSIZE(adapters)];
                DXGIAdapterReleaser(IDXGIAdapter1 *(&adapters)[ARRAYSIZE(adapters)]) : adapters(adapters) {}
                ~DXGIAdapterReleaser() { for(uint32_t i = 0; i < ARRAYSIZE(adapters); ++i) if(adapters[i]) adapters[i]->Release(); }
            };
            DXGIAdapterReleaser const adapter_releaser(adapters);

            uint32_t i = 0;
            for(; i < ARRAYSIZE(adapters); ++i)
                if(!adapters[i]) break; else
                if(SUCCEEDED(D3D12CreateDevice(adapters[i], D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device_))))
                    break;  // we've got a valid device :)
            if(device_ == nullptr)
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create D3D12 device");
            if((flags & kGfxCreateContextFlag_EnableStablePowerState) != 0 && IsDeveloperModeEnabled() && !SUCCEEDED(device_->SetStablePowerState(TRUE)))
            {
                GFX_PRINT_ERROR(kGfxResult_InternalError, "Unable to set stable power state, is developer mode enabled?");
                device_->Release(); device_ = nullptr;  // release crashed device and try to re-create it
                if(!SUCCEEDED(D3D12CreateDevice(adapters[i], D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device_))))
                    return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create D3D12 device");
            }
            adapter_ = adapters[i];
            adapters[i] = nullptr;
        }
        debug_shaders_ = ((flags & kGfxCreateContextFlag_EnableShaderDebugging) != 0);
        cache_shaders_ = ((flags & kGfxCreateContextFlag_EnableShaderCache) != 0);
        device_->QueryInterface(IID_PPV_ARGS(&mesh_device_));
        device_->QueryInterface(IID_PPV_ARGS(&dxr_device_));
        SetDebugName(device_, "gfx_Device");

        if((flags & kGfxCreateContextFlag_EnableDebugLayer) != 0)
        {
            ID3D12InfoQueue1 *debug_callback = nullptr;
            if(SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&debug_callback))))
            {
                DWORD cookie = 0;
                D3D12MessageFunc callback = [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                                               D3D12_MESSAGE_ID, LPCSTR description, void *)
                {
                    if(severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                        GFX_ASSERTMSG(0, "D3D12 error: %s", description);
                    else if(severity == D3D12_MESSAGE_SEVERITY_WARNING)
                        GFX_PRINTLN("D3D12 warning: %s", description);
                };
                debug_callback->RegisterMessageCallback(callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
                debug_callback->Release();
            }
        }

        D3D12_COMMAND_QUEUE_DESC
        queue_desc      = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(!SUCCEEDED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue_))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create command queue");
        command_queue_->GetTimestampFrequency(&timestamp_query_ticks_per_second_);
        SetDebugName(command_queue_, "gfx_CommandQueue");

        max_frames_in_flight_ = kGfxConstant_BackBufferCount;

        command_allocators_ = (ID3D12CommandAllocator **)gfxMalloc(max_frames_in_flight_ * sizeof(ID3D12CommandAllocator *));
        for(uint32_t j = 0; j < max_frames_in_flight_; ++j)
        {
            char buffer[256];
            GFX_SNPRINTF(buffer, sizeof(buffer), "gfx_CommandAllocator%u", j);
            if(!SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators_[j]))))
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create command allocator");
            SetDebugName(command_allocators_[j], buffer);
        }
        if(!SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, *command_allocators_, nullptr, IID_PPV_ARGS(&command_list_))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create command list");
        command_list_->QueryInterface(IID_PPV_ARGS(&dxr_command_list_));
        command_list_->QueryInterface(IID_PPV_ARGS(&mesh_command_list_));
        if(dxr_command_list_ == nullptr) { if(dxr_device_ != nullptr) dxr_device_->Release(); dxr_device_ = nullptr; }
        if(mesh_command_list_ == nullptr) { if(mesh_device_ != nullptr) mesh_device_->Release(); mesh_device_ = nullptr; }
        SetDebugName(command_list_, "gfx_CommandList");
        if((flags & kGfxCreateContextFlag_EnableDebugLayer) != 0)
            command_list_->QueryInterface(IID_PPV_ARGS(&dbg_command_list_));

        fence_event_ = CreateEvent(nullptr, false, false, nullptr);
        if(!fence_event_)
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create event handle");
        fences_ = (ID3D12Fence **)gfxMalloc(max_frames_in_flight_ * sizeof(ID3D12Fence *));
        for(uint32_t j = 0; j < max_frames_in_flight_; ++j)
        {
            char buffer[256];
            GFX_SNPRINTF(buffer, sizeof(buffer), "gfx_Fence%u", j);
            if(!SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences_[j]))))
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create fence object");
            SetDebugName(fences_[j], buffer);
        }
        fence_values_ = (uint64_t *)gfxMalloc(max_frames_in_flight_ * sizeof(uint64_t));
        memset(fence_values_, 0, max_frames_in_flight_ * sizeof(uint64_t));

        return kGfxResult_NoError;
    }

    GfxResult initialize(ID3D12Device *device, uint32_t max_frames_in_flight, GfxContext &context)
    {
        if(!device)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "An invalid D3D12 device was supplied");
        IDXGIFactory4 *factory = nullptr;
        if(!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create DXGI factory");

        if(!SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter_))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to find interop adapter");

        device_ = device;
        is_interop_ = true;
        max_frames_in_flight_ = GFX_MAX(max_frames_in_flight, 1u);
        device->QueryInterface(IID_PPV_ARGS(&dxr_device_));
        device->QueryInterface(IID_PPV_ARGS(&mesh_device_));
        device->AddRef();   // retain device
        factory->Release();

        D3D12_COMMAND_QUEUE_DESC
        queue_desc      = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue *command_queue = nullptr;
        if(!SUCCEEDED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create command queue");
        command_queue->GetTimestampFrequency(&timestamp_query_ticks_per_second_);
        command_queue->Release();

        return initializeCommon(context);
    }

    GfxResult initializeCommon(GfxContext &context)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 rt_features = {};
        device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &rt_features, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5));
        if(rt_features.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) { if(dxr_device_ != nullptr) dxr_device_->Release(); dxr_device_ = nullptr; }
        if(dxr_device_ == nullptr && dxr_command_list_ != nullptr) { dxr_command_list_->Release(); dxr_command_list_ = nullptr; }

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 mesh_features = {};
        device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &mesh_features, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7));
        if(mesh_features.MeshShaderTier < D3D12_MESH_SHADER_TIER_1) { if(mesh_device_ != nullptr) mesh_device_->Release(); mesh_device_ = nullptr; }
        if(mesh_device_ == nullptr && mesh_command_list_ != nullptr) { mesh_command_list_->Release(); mesh_command_list_ = nullptr; }

        D3D12MA::ALLOCATOR_DESC
        allocator_desc          = {};
        allocator_desc.Flags    = D3D12MA::ALLOCATOR_FLAG_SINGLETHREADED;
        allocator_desc.pDevice  = device_;
        allocator_desc.pAdapter = adapter_;
        if(!SUCCEEDED(D3D12MA::CreateAllocator(&allocator_desc, &mem_allocator_)))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create memory allocator");

        for(uint32_t i = 0; i < ARRAYSIZE(dummy_descriptors_); ++i)
        {
            dummy_descriptors_[i] = (i == Kernel::Parameter::kType_Sampler ? allocateSamplerDescriptor() : allocateDescriptor());
            if(dummy_descriptors_[i] == 0xFFFFFFFFu)
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to allocate dummy descriptors");
        }
        dummy_rtv_descriptor_ = allocateRTVDescriptor();
        if(dummy_rtv_descriptor_ == 0xFFFFFFFFu)
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to allocate dummy descriptors");
        {
            D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
            rtv_desc.Format        = back_buffer_format_;
            rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device_->CreateRenderTargetView(nullptr, &rtv_desc, rtv_descriptors_.getCPUHandle(dummy_rtv_descriptor_));
        }
        populateDummyDescriptors();

        constant_buffer_pool_ = (GfxBuffer *)gfxMalloc(max_frames_in_flight_ * sizeof(GfxBuffer));
        constant_buffer_pool_cursors_ = (uint64_t *)gfxMalloc(max_frames_in_flight_ * sizeof(uint64_t));
        timestamp_query_heaps_ = (TimestampQueryHeap *)gfxMalloc(max_frames_in_flight_ * sizeof(TimestampQueryHeap));
        memset(constant_buffer_pool_cursors_, 0, max_frames_in_flight_ * sizeof(uint64_t));
        for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
        {
            new(&constant_buffer_pool_[i]) GfxBuffer();
            new(&timestamp_query_heaps_[i]) TimestampQueryHeap();
        }
        if(!SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils_))) ||
           !SUCCEEDED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler_))) ||
           !SUCCEEDED(dxc_utils_->CreateDefaultIncludeHandler(&dxc_include_handler_)))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create DXC compiler");

        D3D12_INDIRECT_ARGUMENT_DESC dispatch_argument_desc = {};
        dispatch_argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC dispatch_signature_desc = {};
        dispatch_signature_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        dispatch_signature_desc.NumArgumentDescs = 1;
        dispatch_signature_desc.pArgumentDescs = &dispatch_argument_desc;
        if(!SUCCEEDED(device_->CreateCommandSignature(&dispatch_signature_desc, nullptr, IID_PPV_ARGS(&dispatch_signature_))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create the dispatch command signature");

        D3D12_INDIRECT_ARGUMENT_DESC multi_draw_argument_desc = {};
        multi_draw_argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC multi_draw_signature_desc = {};
        multi_draw_signature_desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        multi_draw_signature_desc.NumArgumentDescs = 1;
        multi_draw_signature_desc.pArgumentDescs = &multi_draw_argument_desc;
        if(!SUCCEEDED(device_->CreateCommandSignature(&multi_draw_signature_desc, nullptr, IID_PPV_ARGS(&multi_draw_signature_))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create the multi-draw command signature");

        D3D12_INDIRECT_ARGUMENT_DESC multi_draw_indexed_arguments_desc = {};
        multi_draw_indexed_arguments_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC multi_draw_indexed_signature_desc = {};
        multi_draw_indexed_signature_desc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        multi_draw_indexed_signature_desc.NumArgumentDescs = 1;
        multi_draw_indexed_signature_desc.pArgumentDescs = &multi_draw_indexed_arguments_desc;
        if(!SUCCEEDED(device_->CreateCommandSignature(&multi_draw_indexed_signature_desc, nullptr, IID_PPV_ARGS(&multi_draw_indexed_signature_))))
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create indexed multi-draw command signature");

        if(dxr_device_ != nullptr)
        {
            D3D12_INDIRECT_ARGUMENT_DESC dispatch_rays_argument_desc = {};
            dispatch_rays_argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
            D3D12_COMMAND_SIGNATURE_DESC dispatch_rays_signature_desc = {};
            dispatch_rays_signature_desc.ByteStride = sizeof(D3D12_DISPATCH_RAYS_DESC);
            dispatch_rays_signature_desc.NumArgumentDescs = 1;
            dispatch_rays_signature_desc.pArgumentDescs = &dispatch_rays_argument_desc;
            if(!SUCCEEDED(device_->CreateCommandSignature(&dispatch_rays_signature_desc, nullptr, IID_PPV_ARGS(&dispatch_rays_signature_))))
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create the dispatch rays command signature");
        }

        if(mesh_device_ != nullptr)
        {
            D3D12_INDIRECT_ARGUMENT_DESC draw_mesh_argument_desc = {};
            draw_mesh_argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
            D3D12_COMMAND_SIGNATURE_DESC draw_mesh_signature_desc = {};
            draw_mesh_signature_desc.ByteStride = sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
            draw_mesh_signature_desc.NumArgumentDescs = 1;
            draw_mesh_signature_desc.pArgumentDescs = &draw_mesh_argument_desc;
            if(!SUCCEEDED(device_->CreateCommandSignature(&draw_mesh_signature_desc, nullptr, IID_PPV_ARGS(&draw_mesh_signature_))))
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create the draw mesh command signature");
        }

        GfxProgramDesc clear_buffer_program_desc = {};
        clear_buffer_program_desc.cs = "RWBuffer<uint> OutputBuffer; uint ClearValue; [numthreads(128, 1, 1)] void main(in uint gidx : SV_DispatchThreadID) { OutputBuffer[gidx] = ClearValue; }";
        clear_buffer_program_ = createProgram(clear_buffer_program_desc, "gfx_ClearBufferProgram", nullptr, nullptr, 0);
        clear_buffer_kernel_ = createComputeKernel(clear_buffer_program_, "main", nullptr, 0);
        if(!clear_buffer_kernel_)
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create the compute kernel for clearing buffer objects");

        if(!isInterop())
        {
            GfxDrawState const default_draw_state = {};
            GfxProgramDesc copy_to_backbuffer_program_desc = {};
            copy_to_backbuffer_program_desc.vs = "float4 main(in uint idx : SV_VertexID) : SV_POSITION { return 1.0f - float4(4.0f * (idx & 1), 4.0f * (idx >> 1), 1.0f, 0.0f); }";
            copy_to_backbuffer_program_desc.ps = "Texture2D InputBuffer; float4 main(in float4 pos : SV_Position) : SV_Target { return InputBuffer.Load(int3(pos.xy, 0)); }";
            copy_to_backbuffer_program_ = createProgram(copy_to_backbuffer_program_desc, "gfx_CopyToBackBufferProgram", nullptr, nullptr, 0);
            copy_to_backbuffer_kernel_ = createGraphicsKernel(copy_to_backbuffer_program_, default_draw_state, "main", nullptr, 0);
            if(!copy_to_backbuffer_kernel_)
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create the graphics kernel for copying to the backbuffer");
        }

        DXGI_ADAPTER_DESC1 adapter_desc = {};
        adapter_->GetDesc1(&adapter_desc);
        context.vendor_id = adapter_desc.VendorId;
        GFX_SNPRINTF(context.name, sizeof(context.name), "%ws", adapter_desc.Description);
        GFX_PRINTLN("Created %s `%ws'", isInterop() ? "interop context" : "Direct3D12 device", adapter_desc.Description);
        if(dxr_device_ == nullptr)
            GFX_PRINTLN("Warning: DXR-1.1 is not supported on the selected device; no raytracing will be available");
        bound_viewport_.invalidate(); bound_scissor_rect_.invalidate();

        return kGfxResult_NoError;
    }

    GfxResult terminate()
    {
        destroyBuffer(draw_id_buffer_);
        draw_id_buffer_ = {};
        destroyKernel(clear_buffer_kernel_);
        clear_buffer_kernel_ = {};
        destroyProgram(clear_buffer_program_);
        clear_buffer_program_ = {};
        destroyKernel(copy_to_backbuffer_kernel_);
        copy_to_backbuffer_kernel_ = {};
        destroyProgram(copy_to_backbuffer_program_);
        copy_to_backbuffer_program_ = {};
        destroyBuffer(texture_upload_buffer_);
        texture_upload_buffer_ = {};
        destroyBuffer(raytracing_scratch_buffer_);
        raytracing_scratch_buffer_ = {};
        destroyBuffer(sort_scratch_buffer_);
        sort_scratch_buffer_ = {};

        if(constant_buffer_pool_ != nullptr)
            for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
            {
                destroyBuffer(constant_buffer_pool_[i]);
                constant_buffer_pool_[i].~GfxBuffer();
            }
        gfxFree(constant_buffer_pool_);
        constant_buffer_pool_ = nullptr;
        gfxFree(constant_buffer_pool_cursors_);
        constant_buffer_pool_cursors_ = nullptr;

        for(std::map<uint64_t, Shader>::const_iterator it = shaders_.begin(); it != shaders_.end(); ++it)
        {
            (*it).second.shader_bytecode_->Release();
            (*it).second.shader_reflection_->Release();
        }
        shaders_.clear();
        for(std::map<uint32_t, MipKernels>::const_iterator it = mip_kernels_.begin(); it != mip_kernels_.end(); ++it)
        {
            destroyProgram((*it).second.mip_program_);
            destroyKernel((*it).second.mip_kernel_);
        }
        mip_kernels_.clear();
        for(std::map<uint32_t, ScanKernels>::const_iterator it = scan_kernels_.begin(); it != scan_kernels_.end(); ++it)
        {
            destroyProgram((*it).second.scan_program_);
            destroyKernel((*it).second.reduce_kernel_);
            destroyKernel((*it).second.scan_add_kernel_);
            destroyKernel((*it).second.scan_kernel_);
            destroyKernel((*it).second.args_kernel_);
        }
        scan_kernels_.clear();
        for(std::map<uint32_t, SortKernels>::const_iterator it = sort_kernels_.begin(); it != sort_kernels_.end(); ++it)
        {
            destroyProgram((*it).second.sort_program_);
            destroyKernel((*it).second.histogram_kernel_);
            destroyKernel((*it).second.scatter_kernel_);
            destroyKernel((*it).second.args_kernel_);
        }
        sort_kernels_.clear();

        collect(descriptors_);
        descriptors_.descriptor_heap_        = nullptr;
        descriptors_.descriptor_handle_size_ = 0;
        collect(dsv_descriptors_);
        dsv_descriptors_.descriptor_heap_        = nullptr;
        dsv_descriptors_.descriptor_handle_size_ = 0;
        collect(rtv_descriptors_);
        rtv_descriptors_.descriptor_heap_        = nullptr;
        rtv_descriptors_.descriptor_handle_size_ = 0;
        collect(sampler_descriptors_);
        sampler_descriptors_.descriptor_heap_        = nullptr;
        sampler_descriptors_.descriptor_handle_size_ = 0;

        for(uint32_t i = 0; i < buffers_.size(); ++i)
            collect(buffers_.data()[i]);
        for(uint32_t i = 0; i < textures_.size(); ++i)
            collect(textures_.data()[i]);
        for(uint32_t i = 0; i < sampler_states_.size(); ++i)
            collect(sampler_states_.data()[i]);
        for(uint32_t i = 0; i < acceleration_structures_.size(); ++i)
            collect(acceleration_structures_.data()[i]);
        for(uint32_t i = 0; i < raytracing_primitives_.size(); ++i)
            collect(raytracing_primitives_.data()[i]);
        for(uint32_t i = 0; i < sbts_.size(); ++i)
            collect(sbts_.data()[i]);
        for(uint32_t i = 0; i < kernels_.size(); ++i)
        {
            command_list_->ClearState(kernels_.data()[i].pipeline_state_);
            collect(kernels_.data()[i]);
        }
        for(uint32_t i = 0; i < programs_.size(); ++i)
            collect(programs_.data()[i]);
        if(timestamp_query_heaps_ != nullptr)
            for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
            {
                collect(timestamp_query_heaps_[i].query_heap_);
                destroyBuffer(timestamp_query_heaps_[i].query_buffer_);
                timestamp_query_heaps_[i].~TimestampQueryHeap();
            }
        gfxFree(timestamp_query_heaps_);
        timestamp_query_heaps_ = nullptr;

        forceGarbageCollection();
        buffers_.clear();
        textures_.clear();
        sampler_states_.clear();
        acceleration_structures_.clear();
        raytracing_primitives_.clear();
        sbts_.clear();
        kernels_.clear();
        programs_.clear();
        freelist_descriptors_.clear();
        freelist_dsv_descriptors_.clear();
        freelist_rtv_descriptors_.clear();
        freelist_sampler_descriptors_.clear();

        if(device_ != nullptr)
        {
            device_->Release();
            device_ = nullptr;
        }
        if(dxr_device_ != nullptr)
        {
            dxr_device_->Release();
            dxr_device_ = nullptr;
        }
        if(mesh_device_ != nullptr)
        {
            mesh_device_->Release();
            mesh_device_ = nullptr;
        }
        for(IAmdExtD3DDevice1 *amd_ext_device : amd_ext_devices_)
            amd_ext_device->Release();
        amd_ext_devices_.clear();
        if(command_queue_ != nullptr)
        {
            command_queue_->Release();
            command_queue_ = nullptr;
        }
        if(command_list_ != nullptr)
        {
            command_list_->ClearState(nullptr);
            command_list_->Release();
            command_list_ = nullptr;
        }
        if(dxr_command_list_ != nullptr)
        {
            dxr_command_list_->Release();
            dxr_command_list_ = nullptr;
        }
        if(mesh_command_list_ != nullptr)
        {
            mesh_command_list_->Release();
            mesh_command_list_ = nullptr;
        }
        if(command_allocators_ != nullptr)
            for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
                if(command_allocators_[i] != nullptr)
                    command_allocators_[i]->Release();
        gfxFree(command_allocators_);
        command_allocators_ = nullptr;
        if(dbg_command_list_ != nullptr)
        {
            dbg_command_list_->Release();
            dbg_command_list_ = nullptr;
        }

        if(fence_event_)
        {
            CloseHandle(fence_event_);
            fence_event_ = {};
        }
        if(fences_ != nullptr)
            for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
                if(fences_[i] != nullptr)
                    fences_[i]->Release();
        gfxFree(fence_values_);
        fence_values_ = nullptr;
        gfxFree(fences_);
        fences_ = nullptr;

        if(dxc_utils_ != nullptr)
        {
            dxc_utils_->Release();
            dxc_utils_ = nullptr;
        }
        if(dxc_compiler_ != nullptr)
        {
            dxc_compiler_->Release();
            dxc_compiler_ = nullptr;
        }
        if(dxc_include_handler_ != nullptr)
        {
            dxc_include_handler_->Release();
            dxc_include_handler_ = nullptr;
        }

        if(swap_chain_ != nullptr)
        {
            swap_chain_->Release();
            swap_chain_ = nullptr;
        }
        if(back_buffer_allocations_ != nullptr)
            for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
                if(back_buffer_allocations_[i] != nullptr)
                    back_buffer_allocations_[i]->Release();
        gfxFree(back_buffer_allocations_);
        back_buffer_allocations_ = nullptr;
        if(back_buffers_ != nullptr)
            for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
                if(back_buffers_[i] != nullptr)
                    back_buffers_[i]->Release();
        gfxFree(back_buffer_rtvs_);
        back_buffer_rtvs_ = nullptr;
        gfxFree(back_buffers_);
        back_buffers_ = nullptr;

        if(mem_allocator_ != nullptr)
        {
            mem_allocator_->Release();
            mem_allocator_ = nullptr;
        }
        if(dispatch_signature_ != nullptr)
        {
            dispatch_signature_->Release();
            dispatch_signature_ = nullptr;
        }
        if(multi_draw_signature_ != nullptr)
        {
            multi_draw_signature_->Release();
            multi_draw_signature_ = nullptr;
        }
        if(multi_draw_indexed_signature_ != nullptr)
        {
            multi_draw_indexed_signature_->Release();
            multi_draw_indexed_signature_ = nullptr;
        }
        if(dispatch_rays_signature_ != nullptr)
        {
            dispatch_rays_signature_->Release();
            dispatch_rays_signature_ = nullptr;
        }
        if(draw_mesh_signature_ != nullptr)
        {
            draw_mesh_signature_->Release();
            draw_mesh_signature_ = nullptr;
        }

#ifdef PIX_AMD_EXT
        tls_pAmdExtDeviceObject = nullptr;
        tls_checkAmdDriver = true;
#endif

        return kGfxResult_NoError;
    }

    bool isValid() const
    {
        return device_ != nullptr && mem_allocator_ != nullptr && device_->GetDeviceRemovedReason() == S_OK;
    }

    inline uint32_t getBackBufferWidth() const
    {
        return window_width_;
    }

    inline uint32_t getBackBufferHeight() const
    {
        return window_height_;
    }

    inline uint32_t getBackBufferIndex() const
    {
        return fence_index_;
    }

    inline uint32_t getBackBufferCount() const
    {
        return max_frames_in_flight_;
    }

    inline DXGI_FORMAT getBackBufferFormat() const
    {
        return back_buffer_format_;
    }

    inline DXGI_COLOR_SPACE_TYPE getBackBufferColorSpace() const
    {
        return color_space_;
    }

    inline GfxDisplayDesc getDisplayDescription() const
    {
        IDXGIOutput *output      = nullptr;
        RECT         window_rect = {};
        GetClientRect(window_, &window_rect);
        UINT           output_i  = 0;
        LONG           best_area = -1;
        IDXGIOutput   *current_output;
        while(adapter_->EnumOutputs(output_i, &current_output) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_OUTPUT_DESC output_desc;
            if(SUCCEEDED(current_output->GetDesc(&output_desc))) {
                RECT rect = output_desc.DesktopCoordinates;
                int intersect_area =
                    GFX_MAX(0L, GFX_MIN(window_rect.right, rect.right) - GFX_MAX(window_rect.left, rect.left)) *
                    GFX_MAX(0l, GFX_MIN(window_rect.bottom, rect.bottom) - GFX_MAX(window_rect.top, rect.top));
                if(intersect_area > best_area)
                {
                    output    = current_output;
                    best_area = intersect_area;
                }
            }
            if(current_output != output) current_output->Release();
            output_i++;
        }
        GfxDisplayDesc desc;
        if(output != nullptr)
        {
            IDXGIOutput6 *output6 = nullptr;
            output->QueryInterface(&output6);
            if(output6 != nullptr)
            {
                DXGI_OUTPUT_DESC1 output_desc = {};
                output6->GetDesc1(&output_desc);
                memcpy(&desc.red_primary, output_desc.RedPrimary, sizeof(float) * 2);
                memcpy(&desc.blue_primary, output_desc.BluePrimary, sizeof(float) * 2);
                memcpy(&desc.green_primary, output_desc.GreenPrimary, sizeof(float) * 2);
                memcpy(&desc.white_point, output_desc.WhitePoint, sizeof(float) * 2);
                desc.min_luminance = output_desc.MinLuminance;
                desc.max_luminance = output_desc.MaxLuminance;
                desc.max_luminance_full_frame = output_desc.MaxFullFrameLuminance;

                // Get the monitor name.
                MONITORINFOEXW monitor_info;
                monitor_info.cbSize = sizeof(monitor_info);
                if(GetMonitorInfoW(output_desc.Monitor, &monitor_info))
                {
                    uint32_t num_path_array_elements      = 0;
                    uint32_t num_mode_info_array_elements = 0;
                    if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_path_array_elements, &num_mode_info_array_elements) == ERROR_SUCCESS)
                    {
                        std::vector<DISPLAYCONFIG_PATH_INFO> path_info_list(num_path_array_elements);
                        std::vector<DISPLAYCONFIG_MODE_INFO> mode_info_list(num_mode_info_array_elements);
                        if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_path_array_elements, path_info_list.data(), &num_mode_info_array_elements, mode_info_list.data(), nullptr) == ERROR_SUCCESS)
                        {
                            for(uint32_t i = 0; i < num_path_array_elements; i++)
                            {
                                DISPLAYCONFIG_SOURCE_DEVICE_NAME device_name;
                                device_name.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                                device_name.header.size      = sizeof(device_name);
                                device_name.header.adapterId = path_info_list[i].sourceInfo.adapterId;
                                device_name.header.id        = path_info_list[i].sourceInfo.id;
                                if(DisplayConfigGetDeviceInfo(&device_name.header) == ERROR_SUCCESS)
                                {
                                    if(wcscmp(monitor_info.szDevice, device_name.viewGdiDeviceName) == 0)
                                    {
                                        DISPLAYCONFIG_SDR_WHITE_LEVEL white_level = {};
                                        white_level.header.type =
                                            DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                                        white_level.header.size      = sizeof(white_level);
                                        white_level.header.adapterId = path_info_list[i].targetInfo.adapterId;
                                        white_level.header.id        = path_info_list[i].targetInfo.id;
                                        if(DisplayConfigGetDeviceInfo(&white_level.header) == ERROR_SUCCESS)
                                        {
                                            desc.reference_sdr_white_level = (float)white_level.SDRWhiteLevel * 80.0f / 1000.0f;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                output6->Release();
            }
            output->Release();
        }
        return desc;
    }

    inline bool isRaytracingSupported() const
    {
        return (dxr_device_ != nullptr ? true : false);
    }

    inline bool isMeshShaderSupported() const
    {
        return (mesh_device_ != nullptr ? true : false);
    }

    GfxBuffer createBuffer(uint64_t size, void const *data, GfxCpuAccess cpu_access, D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COMMON)
    {
        GfxBuffer buffer = {};
        if(cpu_access >= kGfxCpuAccess_Count)
            return buffer;  // invalid parameter
        uint32_t const alignment = (cpu_access == kGfxCpuAccess_Write ? 256 : 4);
        D3D12_RESOURCE_DESC
        resource_desc                  = {};
        resource_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource_desc.Width            = GFX_MAX(GFX_ALIGN(size, alignment), (uint64_t)alignment);
        resource_desc.Height           = 1;
        resource_desc.DepthOrArraySize = 1;
        resource_desc.MipLevels        = 1;
        resource_desc.SampleDesc.Count = 1;
        resource_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.handle = buffer_handles_.allocate_handle();
        D3D12MA::ALLOCATION_DESC allocation_desc = {};
        Buffer &gfx_buffer = buffers_.insert(buffer);
        switch(cpu_access)
        {
        case kGfxCpuAccess_Read:
            allocation_desc.HeapType = D3D12_HEAP_TYPE_READBACK;
            resource_state = D3D12_RESOURCE_STATE_COPY_DEST;
            break;
        case kGfxCpuAccess_Write:
            allocation_desc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
            resource_state = D3D12_RESOURCE_STATE_GENERIC_READ;
            break;
        default:    // kGfxCpuAccess_None
            allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            break;
        }
        if(createResource(allocation_desc, resource_desc, resource_state, nullptr, &gfx_buffer.allocation_, IID_PPV_ARGS(&gfx_buffer.resource_)) != kGfxResult_NoError)
        {
            GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to create buffer object of size %u MiB", (uint32_t)((size + 1024 * 1024 - 1) / (1024 * 1024)));
            gfx_buffer.resource_ = nullptr; gfx_buffer.allocation_ = nullptr;
            destroyBuffer(buffer); buffer = {};
            return buffer;
        }
        if(cpu_access != kGfxCpuAccess_None)
        {
            if(cpu_access == kGfxCpuAccess_Write)
            {
                D3D12_RANGE read_range = {};
                gfx_buffer.resource_->Map(0, &read_range, &gfx_buffer.data_);
                memset(gfx_buffer.data_, 0, (size_t)resource_desc.Width);
                if(data) memcpy(gfx_buffer.data_, data, (size_t)size);
            }
            else
                gfx_buffer.resource_->Map(0, nullptr, &gfx_buffer.data_);
        }
        buffer.size = size;
        buffer.cpu_access = cpu_access;
        buffer.stride = sizeof(uint32_t);
        gfx_buffer.reference_count_ = (uint32_t *)gfxMalloc(sizeof(uint32_t));
        GFX_ASSERT(gfx_buffer.reference_count_ != nullptr);
        *gfx_buffer.reference_count_ = 1;   // retain
        gfx_buffer.resource_state_ = (D3D12_RESOURCE_STATES *)gfxMalloc(sizeof(D3D12_RESOURCE_STATES));
        gfx_buffer.transitioned_ = (bool *)gfxMalloc(sizeof(bool));
        GFX_ASSERT(gfx_buffer.resource_state_ != nullptr && gfx_buffer.transitioned_ != nullptr);
        *gfx_buffer.resource_state_ = resource_state;
        *gfx_buffer.transitioned_ = false;
        gfx_buffer.initial_resource_state_ = resource_state;
        if(data != nullptr && cpu_access != kGfxCpuAccess_Write)
        {
            GfxBuffer const upload_buffer = createBuffer(size, data, kGfxCpuAccess_Write);
            encodeCopyBuffer(buffer, upload_buffer);
            destroyBuffer(upload_buffer);
        }
        return buffer;
    }

    GfxBuffer createBufferRange(GfxBuffer const &buffer, uint64_t byte_offset, uint64_t size)
    {
        GfxBuffer buffer_range = {};
        if(!buffer_handles_.has_handle(buffer.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create a buffer range from an invalid buffer object");
            return buffer_range;
        }
        if(byte_offset + size > buffer.size)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a buffer range that is larger than the original buffer object");
            return buffer_range;
        }
        uint32_t const alignment = (buffer.cpu_access == kGfxCpuAccess_Write ? 256 : 4);
        if(byte_offset != GFX_ALIGN(byte_offset, alignment))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Buffer objects with %s CPU access must be %u-byte aligned; cannot create range starting at offset %llu",
                buffer.cpu_access == kGfxCpuAccess_Read ? "read" : buffer.cpu_access == kGfxCpuAccess_Write ? "write" : "no", alignment, byte_offset);
            return buffer_range;
        }
        Buffer const gfx_buffer = buffers_[buffer];
        if(size == 0) size = (buffer.size - byte_offset);
        buffer_range.handle = buffer_handles_.allocate_handle();
        Buffer &gfx_buffer_range = buffers_.insert(buffer_range, gfx_buffer);
        if(gfx_buffer_range.data_ != nullptr) gfx_buffer_range.data_ = (char *)gfx_buffer_range.data_ + byte_offset;
        GFX_ASSERT(gfx_buffer_range.resource_ != nullptr && gfx_buffer_range.reference_count_ != nullptr);
        gfx_buffer_range.data_offset_ += byte_offset;
        ++*gfx_buffer_range.reference_count_;   // retain
        buffer_range.cpu_access = buffer.cpu_access;
        strcpy(buffer_range.name, buffer.name);
        buffer_range.stride = buffer.stride;
        buffer_range.size = size;
        return buffer_range;
    }

    GfxResult destroyBuffer(GfxBuffer const &buffer)
    {
        if(!buffer)
            return kGfxResult_NoError;
        if(!buffer_handles_.has_handle(buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot destroy invalid buffer object");
        collect(buffers_[buffer]);  // release resources
        buffers_.erase(buffer); // destroy buffer
        buffer_handles_.free_handle(buffer.handle);
        return kGfxResult_NoError;
    }

    void *getBufferData(GfxBuffer const &buffer)
    {
        if(!buffer_handles_.has_handle(buffer.handle))
            return nullptr; // invalid buffer object
        Buffer const &gfx_buffer = buffers_[buffer];
        return gfx_buffer.data_;
    }

    GfxTexture createTexture2D(DXGI_FORMAT format, float const *clear_value)
    {
        return createTexture2D(window_width_, window_height_, format, 1, clear_value, Texture::kFlag_AutoResize);
    }

    GfxTexture createTexture2D(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t mip_levels, float const *clear_value, uint32_t flags = 0)
    {
        GfxTexture texture = {};
        if(isInterop() && (flags & Texture::kFlag_AutoResize) != 0)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create auto-resize texture objects when using an interop context");
            return texture; // invalid operation
        }
        if(format == DXGI_FORMAT_UNKNOWN || format == DXGI_FORMAT_FORCE_UINT)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create texture object using invalid format");
            return texture; // invalid parameter
        }
        width = GFX_MAX(width, 1u);
        height = GFX_MAX(height, 1u);
        mip_levels = GFX_MIN(GFX_MAX(mip_levels, 1u), gfxCalculateMipCount(width, height));
        D3D12_RESOURCE_STATES const resource_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_DESC
        resource_desc                  = {};
        resource_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_desc.Width            = width;
        resource_desc.Height           = height;
        resource_desc.DepthOrArraySize = 1;
        resource_desc.MipLevels        = (uint16_t)mip_levels;
        resource_desc.Format           = format;
        resource_desc.SampleDesc.Count = 1;
        texture.handle = texture_handles_.allocate_handle();
        Texture &gfx_texture = textures_.insert(texture);
        D3D12MA::ALLOCATION_DESC allocation_desc = {};
        allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ResolveClearValueForTexture(gfx_texture, clear_value, format);
        if(createResource(allocation_desc, resource_desc, resource_state, gfx_texture.clear_value_, &gfx_texture.allocation_, IID_PPV_ARGS(&gfx_texture.resource_)) != kGfxResult_NoError)
        {
            GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to create 2D texture object of size %ux%u", width, height);
            gfx_texture.resource_ = nullptr; gfx_texture.allocation_ = nullptr;
            destroyTexture(texture); texture = {};
            return texture;
        }
        texture.format = format;
        texture.mip_levels = mip_levels;
        texture.type = GfxTexture::kType_2D;
        texture.depth = 1;
        if(!((flags & Texture::kFlag_AutoResize) != 0))
        {
            texture.width = width;
            texture.height = height;
        }
        memcpy(texture.clear_value, gfx_texture.clear_value_, sizeof(texture.clear_value));
        gfx_texture.resource_state_ = resource_state;
        gfx_texture.initial_resource_state_ = resource_state;
        gfx_texture.transitioned_ = false;
        gfx_texture.flags_ = flags;
        return texture;
    }

    GfxTexture createTexture2DArray(uint32_t width, uint32_t height, uint32_t slice_count, DXGI_FORMAT format, uint32_t mip_levels, float const *clear_value)
    {
        GfxTexture texture = {};
        if(format == DXGI_FORMAT_UNKNOWN || format == DXGI_FORMAT_FORCE_UINT)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create texture object using invalid format");
            return texture; // invalid parameter
        }
        if(slice_count > 0xFFFFu)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a 2D texture array with more than %u slices", 0xFFFFu);
            return texture; // invalid operation
        }
        width = GFX_MAX(width, 1u);
        height = GFX_MAX(height, 1u);
        slice_count = GFX_MAX(slice_count, 1u);
        mip_levels = GFX_MIN(GFX_MAX(mip_levels, 1u), gfxCalculateMipCount(width, height));
        D3D12_RESOURCE_STATES const resource_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_DESC
        resource_desc                  = {};
        resource_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_desc.Width            = width;
        resource_desc.Height           = height;
        resource_desc.DepthOrArraySize = (uint16_t)slice_count;
        resource_desc.MipLevels        = (uint16_t)mip_levels;
        resource_desc.Format           = format;
        resource_desc.SampleDesc.Count = 1;
        texture.handle = texture_handles_.allocate_handle();
        Texture &gfx_texture = textures_.insert(texture);
        D3D12MA::ALLOCATION_DESC allocation_desc = {};
        allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ResolveClearValueForTexture(gfx_texture, clear_value, format);
        if(createResource(allocation_desc, resource_desc, resource_state, gfx_texture.clear_value_, &gfx_texture.allocation_, IID_PPV_ARGS(&gfx_texture.resource_)) != kGfxResult_NoError)
        {
            GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to create 2D texture array object of size %ux%ux%u", width, height, slice_count);
            gfx_texture.resource_ = nullptr; gfx_texture.allocation_ = nullptr;
            destroyTexture(texture); texture = {};
            return texture;
        }
        texture.format = format;
        texture.mip_levels = mip_levels;
        texture.type = GfxTexture::kType_2DArray;
        texture.width = width;
        texture.height = height;
        texture.depth = slice_count;
        memcpy(texture.clear_value, gfx_texture.clear_value_, sizeof(texture.clear_value));
        gfx_texture.resource_state_ = resource_state;
        gfx_texture.initial_resource_state_ = resource_state;
        gfx_texture.transitioned_ = false;
        return texture;
    }

    GfxTexture createTexture3D(uint32_t width, uint32_t height, uint32_t depth, DXGI_FORMAT format, uint32_t mip_levels, float const *clear_value)
    {
        GfxTexture texture = {};
        if(format == DXGI_FORMAT_UNKNOWN || format == DXGI_FORMAT_FORCE_UINT)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create texture object using invalid format");
            return texture; // invalid parameter
        }
        if(depth > 0xFFFFu)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a 3D texture with a depth larger than %u", 0xFFFFu);
            return texture; // invalid operation
        }
        width = GFX_MAX(width, 1u);
        height = GFX_MAX(height, 1u);
        depth = GFX_MAX(depth, 1u);
        mip_levels = GFX_MIN(GFX_MAX(mip_levels, 1u), gfxCalculateMipCount(width, height, depth));
        D3D12_RESOURCE_STATES const resource_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_DESC
        resource_desc                  = {};
        resource_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        resource_desc.Width            = width;
        resource_desc.Height           = height;
        resource_desc.DepthOrArraySize = (uint16_t)depth;
        resource_desc.MipLevels        = (uint16_t)mip_levels;
        resource_desc.Format           = format;
        resource_desc.SampleDesc.Count = 1;
        texture.handle = texture_handles_.allocate_handle();
        Texture &gfx_texture = textures_.insert(texture);
        D3D12MA::ALLOCATION_DESC allocation_desc = {};
        allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ResolveClearValueForTexture(gfx_texture, clear_value, format);
        if(createResource(allocation_desc, resource_desc, resource_state, gfx_texture.clear_value_, &gfx_texture.allocation_, IID_PPV_ARGS(&gfx_texture.resource_)) != kGfxResult_NoError)
        {
            GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to create 3D texture object of size %ux%ux%u", width, height, depth);
            gfx_texture.resource_ = nullptr; gfx_texture.allocation_ = nullptr;
            destroyTexture(texture); texture = {};
            return texture;
        }
        texture.format = format;
        texture.mip_levels = mip_levels;
        texture.type = GfxTexture::kType_3D;
        texture.width = width;
        texture.height = height;
        texture.depth = depth;
        memcpy(texture.clear_value, gfx_texture.clear_value_, sizeof(texture.clear_value));
        gfx_texture.resource_state_ = resource_state;
        gfx_texture.initial_resource_state_ = resource_state;
        gfx_texture.transitioned_ = false;
        return texture;
    }

    GfxTexture createTextureCube(uint32_t size, DXGI_FORMAT format, uint32_t mip_levels, float const *clear_value)
    {
        GfxTexture texture = {};
        if(format == DXGI_FORMAT_UNKNOWN || format == DXGI_FORMAT_FORCE_UINT)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create texture object using invalid format");
            return texture; // invalid parameter
        }
        size = GFX_MAX(size, 1u);
        mip_levels = GFX_MIN(GFX_MAX(mip_levels, 1u), gfxCalculateMipCount(size));
        D3D12_RESOURCE_STATES const resource_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_DESC
        resource_desc                  = {};
        resource_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_desc.Width            = size;
        resource_desc.Height           = size;
        resource_desc.DepthOrArraySize = 6;
        resource_desc.MipLevels        = (uint16_t)mip_levels;
        resource_desc.Format           = format;
        resource_desc.SampleDesc.Count = 1;
        texture.handle = texture_handles_.allocate_handle();
        Texture &gfx_texture = textures_.insert(texture);
        D3D12MA::ALLOCATION_DESC allocation_desc = {};
        allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ResolveClearValueForTexture(gfx_texture, clear_value, format);
        if(createResource(allocation_desc, resource_desc, resource_state, gfx_texture.clear_value_, &gfx_texture.allocation_, IID_PPV_ARGS(&gfx_texture.resource_)) != kGfxResult_NoError)
        {
            GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to create cubemap texture object of size %u", size);
            gfx_texture.resource_ = nullptr; gfx_texture.allocation_ = nullptr;
            destroyTexture(texture); texture = {};
            return texture;
        }
        texture.format = format;
        texture.mip_levels = mip_levels;
        texture.type = GfxTexture::kType_Cube;
        texture.width = size;
        texture.height = size;
        texture.depth = 6;
        memcpy(texture.clear_value, gfx_texture.clear_value_, sizeof(texture.clear_value));
        gfx_texture.resource_state_ = resource_state;
        gfx_texture.initial_resource_state_ = resource_state;
        gfx_texture.transitioned_ = false;
        return texture;
    }

    GfxResult destroyTexture(GfxTexture const &texture)
    {
        if(!texture)
            return kGfxResult_NoError;
        if(!texture_handles_.has_handle(texture.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot destroy invalid texture object");
        collect(textures_[texture]);    // release resources
        textures_.erase(texture);   // destroy texture object
        texture_handles_.free_handle(texture.handle);
        return kGfxResult_NoError;
    }

    GfxSamplerState createSamplerState(D3D12_FILTER filter,
                                       D3D12_TEXTURE_ADDRESS_MODE address_u,
                                       D3D12_TEXTURE_ADDRESS_MODE address_v,
                                       D3D12_TEXTURE_ADDRESS_MODE address_w,
                                       float mip_lod_bias, float min_lod, float max_lod)
    {
        GfxSamplerState const sampler_state = {};
        if(D3D12_DECODE_IS_COMPARISON_FILTER(filter))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Comparison samplers require a valid comparison function; cannot create sampler state object");
            return sampler_state;
        }
        return createSamplerState(filter, (D3D12_COMPARISON_FUNC)0, address_u, address_v, address_w, mip_lod_bias, min_lod, max_lod);
    }

    GfxSamplerState createSamplerState(D3D12_FILTER filter,
                                       D3D12_COMPARISON_FUNC comparison_func,
                                       D3D12_TEXTURE_ADDRESS_MODE address_u,
                                       D3D12_TEXTURE_ADDRESS_MODE address_v,
                                       D3D12_TEXTURE_ADDRESS_MODE address_w,
                                       float mip_lod_bias, float min_lod, float max_lod)
    {
        GfxSamplerState sampler_state = {};
        sampler_state.handle = sampler_state_handles_.allocate_handle();
        SamplerState &gfx_sampler_state = sampler_states_.insert(sampler_state);
        gfx_sampler_state.descriptor_slot_ = allocateSamplerDescriptor();
        if(gfx_sampler_state.descriptor_slot_ == 0xFFFFFFFFu)
        {
            GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to allocate descriptor for sampler state object");
            destroySamplerState(sampler_state); sampler_state = {};
            return sampler_state;
        }
        gfx_sampler_state.sampler_desc_.Filter = filter;
        gfx_sampler_state.sampler_desc_.AddressU = address_u;
        gfx_sampler_state.sampler_desc_.AddressV = address_v;
        gfx_sampler_state.sampler_desc_.AddressW = address_w;
        gfx_sampler_state.sampler_desc_.MipLODBias = mip_lod_bias;
        gfx_sampler_state.sampler_desc_.MaxAnisotropy = (D3D12_DECODE_IS_ANISOTROPIC_FILTER(filter) ? kGfxConstant_MaxAnisotropy : 0);
        gfx_sampler_state.sampler_desc_.ComparisonFunc = comparison_func;
        gfx_sampler_state.sampler_desc_.MinLOD = min_lod;
        gfx_sampler_state.sampler_desc_.MaxLOD = max_lod;
        device_->CreateSampler(&gfx_sampler_state.sampler_desc_, sampler_descriptors_.getCPUHandle(gfx_sampler_state.descriptor_slot_));
        sampler_state.max_anisotropy = gfx_sampler_state.sampler_desc_.MaxAnisotropy;
        sampler_state.comparison_func = comparison_func;
        sampler_state.mip_lod_bias = mip_lod_bias;
        sampler_state.address_u = address_u;
        sampler_state.address_v = address_v;
        sampler_state.address_w = address_w;
        sampler_state.min_lod = min_lod;
        sampler_state.max_lod = max_lod;
        sampler_state.filter = filter;
        return sampler_state;
    }

    GfxResult destroySamplerState(GfxSamplerState const &sampler_state)
    {
        if(!sampler_state)
            return kGfxResult_NoError;
        if(!sampler_state_handles_.has_handle(sampler_state.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot destroy invalid sampler state object");
        collect(sampler_states_[sampler_state]);    // release resources
        sampler_states_.erase(sampler_state);   // destroy sampler state
        sampler_state_handles_.free_handle(sampler_state.handle);
        return kGfxResult_NoError;
    }

    GfxAccelerationStructure createAccelerationStructure()
    {
        GfxAccelerationStructure acceleration_structure = {};
        if(dxr_device_ == nullptr)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Raytracing isn't supported on the selected device; cannot create acceleration structure");
            return acceleration_structure;  // invalid operation
        }
        acceleration_structure.handle = acceleration_structure_handles_.allocate_handle();
        acceleration_structures_.insert(acceleration_structure);
        return acceleration_structure;
    }

    GfxResult destroyAccelerationStructure(GfxAccelerationStructure const &acceleration_structure)
    {
        if(!acceleration_structure)
            return kGfxResult_NoError;
        if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot destroy invalid acceleration structure object");
        collect(acceleration_structures_[acceleration_structure]);  // release resources
        acceleration_structures_.erase(acceleration_structure); // destroy acceleration structure
        acceleration_structure_handles_.free_handle(acceleration_structure.handle);
        return kGfxResult_NoError;
    }

    GfxResult updateAccelerationStructure(GfxAccelerationStructure const &acceleration_structure)
    {
        void *data = nullptr;
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(isInterop(acceleration_structure))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update an interop acceleration structure object");
        if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot update an invalid acceleration structure object");
        AccelerationStructure &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
        if(!gfx_acceleration_structure.needs_update_ && !gfx_acceleration_structure.needs_rebuild_)
            return kGfxResult_NoError;  // no outstanding build requests, early out
        D3D12_GPU_VIRTUAL_ADDRESS const gpu_addr = allocateConstantMemory(gfx_acceleration_structure.raytracing_primitives_.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), data);
        D3D12_RAYTRACING_INSTANCE_DESC *instance_descs = (D3D12_RAYTRACING_INSTANCE_DESC *)data;
        uint32_t instance_desc_count = 0;
        if(gpu_addr == 0)
            return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to allocate for updating acceleration structure object with %u raytracing primitives", (uint32_t)gfx_acceleration_structure.raytracing_primitives_.size());
        active_raytracing_primitives_.reserve(GFX_MAX(active_raytracing_primitives_.capacity(), gfx_acceleration_structure.raytracing_primitives_.size()));
        active_raytracing_primitives_.clear();  // compact away discarded raytracing primitives
        for(size_t i = 0; i < gfx_acceleration_structure.raytracing_primitives_.size(); ++i)
        {
            GfxRaytracingPrimitive const &raytracing_primitive = gfx_acceleration_structure.raytracing_primitives_[i];
            if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
                continue;   // invalid raytracing primitive object
            active_raytracing_primitives_.push_back(raytracing_primitive);
            RaytracingPrimitive const &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
            GfxBuffer const &buffer = getRaytracingPrimitiveBuffer(gfx_raytracing_primitive);
            if(!buffer_handles_.has_handle(buffer.handle))
                continue;   // no valid BVH memory, probably wasn't built
            D3D12_RAYTRACING_INSTANCE_DESC instance_desc = {};
            Buffer const &gfx_buffer = buffers_[buffer];
            for(uint32_t row = 0; row < 3; ++row)
                for(uint32_t col = 0; col < 3; ++col)
                    instance_desc.Transform[row][col] = gfx_raytracing_primitive.transform_[4 * row + col];
            for(uint32_t j = 0; j < 3; ++j)
                instance_desc.Transform[j][3] = gfx_raytracing_primitive.transform_[4 * j + 3];
            instance_desc.InstanceID = gfx_raytracing_primitive.instance_id_;
            instance_desc.InstanceMask = gfx_raytracing_primitive.instance_mask_;
            instance_desc.InstanceContributionToHitGroupIndex = gfx_raytracing_primitive.instance_contribution_to_hit_group_index_;
            instance_desc.AccelerationStructure = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
            instance_descs[instance_desc_count++] = instance_desc;
        }
        std::swap(active_raytracing_primitives_, gfx_acceleration_structure.raytracing_primitives_);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs = {};
        tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        tlas_inputs.NumDescs = instance_desc_count;
        tlas_inputs.InstanceDescs = gpu_addr;
        if(!gfx_acceleration_structure.needs_rebuild_ && gfx_acceleration_structure.bvh_buffer_)
            tlas_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        gfx_acceleration_structure.needs_update_ = gfx_acceleration_structure.needs_rebuild_ = false;
        if(instance_desc_count == 0)
        {
            destroyBuffer(gfx_acceleration_structure.bvh_buffer_);
            gfx_acceleration_structure.bvh_buffer_ = {};
            return kGfxResult_NoError;
        }
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info = {};
        dxr_device_->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_inputs, &tlas_info);
        uint64_t const scratch_data_size = GFX_MAX(tlas_info.ScratchDataSizeInBytes, tlas_info.UpdateScratchDataSizeInBytes);
        GFX_TRY(allocateRaytracingScratch(scratch_data_size));  // ensure scratch is large enough
        uint64_t const bvh_data_size = GFX_ALIGN(tlas_info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        if(bvh_data_size > gfx_acceleration_structure.bvh_buffer_.size)
        {
            destroyBuffer(gfx_acceleration_structure.bvh_buffer_);
            tlas_inputs.Flags &= ~D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            gfx_acceleration_structure.bvh_buffer_ = createBuffer(bvh_data_size, nullptr, kGfxCpuAccess_None, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            if(!gfx_acceleration_structure.bvh_buffer_)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to create acceleration structure buffer");
        }
        gfx_acceleration_structure.bvh_data_size_ = (uint64_t)tlas_info.ResultDataMaxSizeInBytes;
        GFX_ASSERT(buffer_handles_.has_handle(gfx_acceleration_structure.bvh_buffer_.handle));
        GFX_ASSERT(buffer_handles_.has_handle(raytracing_scratch_buffer_.handle));
        Buffer &gfx_buffer = buffers_[gfx_acceleration_structure.bvh_buffer_];
        Buffer &gfx_scratch_buffer = buffers_[raytracing_scratch_buffer_];
        SetObjectName(gfx_buffer, acceleration_structure.name);
        if(transitionResource(gfx_scratch_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
            submitPipelineBarriers();   // ensure scratch is not in use
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc = {};
        build_desc.DestAccelerationStructureData = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
        build_desc.Inputs = tlas_inputs;
        if((tlas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0)
            build_desc.SourceAccelerationStructureData = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
        build_desc.ScratchAccelerationStructureData = gfx_scratch_buffer.resource_->GetGPUVirtualAddress() + gfx_scratch_buffer.data_offset_;
        GFX_ASSERT(dxr_command_list_ != nullptr);   // should never happen
        dxr_command_list_->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);
        return kGfxResult_NoError;
    }

    uint64_t getAccelerationStructureDataSize(GfxAccelerationStructure const &acceleration_structure)
    {
        if(dxr_device_ == nullptr) return 0;    // avoid spamming console output
        if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot get the data size of an invalid acceleration structure object");
            return 0;
        }
        AccelerationStructure const &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
        return gfx_acceleration_structure.bvh_data_size_;
    }

    GfxRaytracingPrimitive createRaytracingPrimitive(GfxAccelerationStructure const &acceleration_structure)
    {
        GfxRaytracingPrimitive raytracing_primitive = {};
        if(dxr_device_ == nullptr)
            return raytracing_primitive;    // avoid spamming console output
        if(isInterop(acceleration_structure))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a raytracing primitive using an interop acceleration structure object");
            return raytracing_primitive;
        }
        if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create a raytracing primitive using an invalid acceleration structure object");
            return raytracing_primitive;
        }
        raytracing_primitive.type = GfxRaytracingPrimitive::kType_Triangles;
        raytracing_primitive.handle = raytracing_primitive_handles_.allocate_handle();
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_.insert(raytracing_primitive);
        AccelerationStructure &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
        gfx_raytracing_primitive.index_ = (uint32_t)gfx_acceleration_structure.raytracing_primitives_.size();
        for(uint32_t i = 0; i < ARRAYSIZE(gfx_raytracing_primitive.transform_); ++i)
            gfx_raytracing_primitive.transform_[i] = ((i & 3) == (i >> 2) ? 1.0f : 0.0f);
        gfx_acceleration_structure.raytracing_primitives_.push_back(raytracing_primitive);
        gfx_raytracing_primitive.triangles_.acceleration_structure_ = acceleration_structure;
        gfx_raytracing_primitive.instance_id_ = raytracing_primitive.getIndex();
        gfx_raytracing_primitive.type_ = RaytracingPrimitive::kType_Triangles;
        gfx_acceleration_structure.needs_rebuild_ = true;
        return raytracing_primitive;
    }

    GfxRaytracingPrimitive createRaytracingPrimitiveInstance(GfxRaytracingPrimitive raytracing_primitive)
    {
        GfxRaytracingPrimitive cloned_raytracing_primitive = {};
        if(dxr_device_ == nullptr)
            return cloned_raytracing_primitive; // avoid spamming console output
        for(;;)
        {
            if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            {
                GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create a raytracing primitive using an invalid raytracing primitive object");
                return cloned_raytracing_primitive;
            }
            RaytracingPrimitive const &parent_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
            if(parent_raytracing_primitive.type_ != RaytracingPrimitive::kType_Instance)
                break;  // found parent raytracing primitive
            raytracing_primitive = parent_raytracing_primitive.instance_.parent_;
            break;
        }
        RaytracingPrimitive const &parent_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        GfxAccelerationStructure const &acceleration_structure = getRaytracingPrimitiveAccelerationStructure(parent_raytracing_primitive);
        GFX_ASSERT(!isInterop(acceleration_structure)); // should never happen
        if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create a raytracing primitive using an invalid acceleration structure object");
            return cloned_raytracing_primitive;
        }
        cloned_raytracing_primitive.type = GfxRaytracingPrimitive::kType_Instance;
        cloned_raytracing_primitive.handle = raytracing_primitive_handles_.allocate_handle();
        AccelerationStructure &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_.insert(cloned_raytracing_primitive);
        gfx_raytracing_primitive.index_ = (uint32_t)gfx_acceleration_structure.raytracing_primitives_.size();
        for(uint32_t i = 0; i < ARRAYSIZE(gfx_raytracing_primitive.transform_); ++i)
            gfx_raytracing_primitive.transform_[i] = ((i & 3) == (i >> 2) ? 1.0f : 0.0f);
        gfx_acceleration_structure.raytracing_primitives_.push_back(cloned_raytracing_primitive);
        gfx_raytracing_primitive.instance_id_ = cloned_raytracing_primitive.getIndex();
        gfx_raytracing_primitive.type_ = RaytracingPrimitive::kType_Instance;
        gfx_raytracing_primitive.instance_.parent_ = raytracing_primitive;
        gfx_acceleration_structure.needs_rebuild_ = true;
        return cloned_raytracing_primitive;
    }

    GfxRaytracingPrimitive createRaytracingPrimitiveProcedural(GfxAccelerationStructure const &acceleration_structure)
    {
        GfxRaytracingPrimitive raytracing_primitive = {};
        if(dxr_device_ == nullptr)
            return raytracing_primitive;    // avoid spamming console output
        if(isInterop(acceleration_structure))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a raytracing primitive using an interop acceleration structure object");
            return raytracing_primitive;
        }
        if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create a raytracing primitive using an invalid acceleration structure object");
            return raytracing_primitive;
        }
        raytracing_primitive.type = GfxRaytracingPrimitive::kType_Procedural;
        raytracing_primitive.handle = raytracing_primitive_handles_.allocate_handle();
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_.insert(raytracing_primitive);
        AccelerationStructure &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
        gfx_raytracing_primitive.index_ = (uint32_t)gfx_acceleration_structure.raytracing_primitives_.size();
        for(uint32_t i = 0; i < ARRAYSIZE(gfx_raytracing_primitive.transform_); ++i)
            gfx_raytracing_primitive.transform_[i] = ((i & 3) == (i >> 2) ? 1.0f : 0.0f);
        gfx_acceleration_structure.raytracing_primitives_.push_back(raytracing_primitive);
        gfx_raytracing_primitive.procedural_.acceleration_structure_ = acceleration_structure;
        gfx_raytracing_primitive.instance_id_ = raytracing_primitive.getIndex();
        gfx_raytracing_primitive.type_ = RaytracingPrimitive::kType_Procedural;
        gfx_acceleration_structure.needs_rebuild_ = true;
        return raytracing_primitive;
    }

    GfxResult destroyRaytracingPrimitive(GfxRaytracingPrimitive const &raytracing_primitive)
    {
        if(!raytracing_primitive)
            return kGfxResult_NoError;
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot destroy invalid raytracing primitive object");
        RaytracingPrimitive const &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        GfxAccelerationStructure const &acceleration_structure = getRaytracingPrimitiveAccelerationStructure(gfx_raytracing_primitive);
        if(acceleration_structure_handles_.has_handle(acceleration_structure.handle))
        {
            acceleration_structures_[acceleration_structure].needs_rebuild_ = true;
            auto &primitives = acceleration_structures_[acceleration_structure].raytracing_primitives_;
            if(gfx_raytracing_primitive.index_ < primitives.size() && primitives[gfx_raytracing_primitive.index_].handle == raytracing_primitive.handle)
            {
                auto it = primitives.begin() + gfx_raytracing_primitive.index_;
                primitives.erase(it);
            }
        }
        collect(gfx_raytracing_primitive);  // release resources
        raytracing_primitives_.erase(raytracing_primitive); // destroy raytracing primitive
        raytracing_primitive_handles_.free_handle(raytracing_primitive.handle);
        return kGfxResult_NoError;
    }

    GfxResult buildRaytracingPrimitive(GfxRaytracingPrimitive const &raytracing_primitive, GfxBuffer const &vertex_buffer, uint32_t vertex_stride, GfxBuildRaytracingPrimitiveFlags build_flags)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set geometry on an invalid raytracing primitive object");
        if(!buffer_handles_.has_handle(vertex_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot build a raytracing primitive using an invalid vertex buffer object");
        vertex_stride = (vertex_stride != 0 ? vertex_stride : vertex_buffer.stride);
        if(vertex_stride == 0)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a raytracing primitive with a vertex buffer object of stride `0'");
        if(vertex_buffer.size / vertex_stride > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a raytracing primitive with a buffer object containing more than 4 billion vertices");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        if(gfx_raytracing_primitive.type_ != RaytracingPrimitive::kType_Triangles)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a non-triangle raytracing primitive object");
        destroyBuffer(gfx_raytracing_primitive.triangles_.index_buffer_);
        destroyBuffer(gfx_raytracing_primitive.triangles_.vertex_buffer_);
        gfx_raytracing_primitive.triangles_.build_flags_ = (uint32_t)build_flags;
        gfx_raytracing_primitive.triangles_.index_buffer_ = {};
        gfx_raytracing_primitive.triangles_.index_stride_ = 0;
        gfx_raytracing_primitive.triangles_.vertex_buffer_ = createBufferRange(vertex_buffer, 0, vertex_buffer.size);
        gfx_raytracing_primitive.triangles_.vertex_stride_ = vertex_stride;
        return buildRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive, false);
    }

    GfxResult buildRaytracingPrimitive(GfxRaytracingPrimitive const &raytracing_primitive, GfxBuffer const &index_buffer, GfxBuffer const &vertex_buffer, uint32_t vertex_stride, GfxBuildRaytracingPrimitiveFlags build_flags)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set geometry on an invalid raytracing primitive object");
        if(!buffer_handles_.has_handle(index_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot build a raytracing primitive using an invalid index buffer object");
        if(!buffer_handles_.has_handle(vertex_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot build a raytracing primitive using an invalid vertex buffer object");
        uint32_t const index_stride = (index_buffer.stride == 2 ? 2 : 4);
        if(index_buffer.size / index_stride > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a raytracing primitive with a buffer object containing more than 4 billion indices");
        vertex_stride = (vertex_stride != 0 ? vertex_stride : vertex_buffer.stride);
        if(vertex_stride == 0)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a raytracing primitive with a vertex buffer object of stride `0'");
        if(vertex_buffer.size / vertex_stride > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a raytracing primitive with a buffer object containing more than 4 billion vertices");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        if(gfx_raytracing_primitive.type_ != RaytracingPrimitive::kType_Triangles)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a non-triangle raytracing primitive object");
        destroyBuffer(gfx_raytracing_primitive.triangles_.index_buffer_);
        destroyBuffer(gfx_raytracing_primitive.triangles_.vertex_buffer_);
        gfx_raytracing_primitive.triangles_.build_flags_ = (uint32_t)build_flags;
        gfx_raytracing_primitive.triangles_.index_buffer_ = createBufferRange(index_buffer, 0, index_buffer.size);
        gfx_raytracing_primitive.triangles_.index_stride_ = index_stride;
        gfx_raytracing_primitive.triangles_.vertex_buffer_ = createBufferRange(vertex_buffer, 0, vertex_buffer.size);
        gfx_raytracing_primitive.triangles_.vertex_stride_ = vertex_stride;
        return buildRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive, false);
    }

    GfxResult buildRaytracingPrimitiveProcedural(GfxRaytracingPrimitive const &raytracing_primitive, GfxBuffer const &aabb_buffer, uint32_t aabb_stride, GfxBuildRaytracingPrimitiveFlags build_flags)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set geometry on an invalid raytracing primitive object");
        if(!buffer_handles_.has_handle(aabb_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot build a raytracing primitive using an invalid AABB buffer object");
        aabb_stride = (aabb_stride != 0 ? aabb_stride : aabb_buffer.stride);
        if(aabb_stride == 0)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a raytracing primitive with an AABB buffer object of stride `0'");
        if(aabb_buffer.size / aabb_stride > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a raytracing primitive with a buffer object containing more than 4 billion AABBs");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        if(gfx_raytracing_primitive.type_ != RaytracingPrimitive::kType_Procedural)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a non-procedural raytracing primitive object");
        destroyBuffer(gfx_raytracing_primitive.procedural_.procedural_buffer_);
        gfx_raytracing_primitive.procedural_.build_flags_ = (uint32_t)build_flags;
        gfx_raytracing_primitive.procedural_.procedural_buffer_ = createBufferRange(aabb_buffer, 0, aabb_buffer.size);
        gfx_raytracing_primitive.procedural_.procedural_stride_ = aabb_stride;
        gfx_raytracing_primitive.type_ = RaytracingPrimitive::kType_Procedural;
        return buildRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive, false);
    }

    GfxResult setRaytracingPrimitiveTransform(GfxRaytracingPrimitive const &raytracing_primitive, float const *row_major_4x4_transform)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set transform on an invalid raytracing primitive object");
        if(row_major_4x4_transform == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot pass `nullptr' as the transform of a raytracing primitive object");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        GFX_TRY(updateRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive));
        memcpy(gfx_raytracing_primitive.transform_, row_major_4x4_transform, sizeof(gfx_raytracing_primitive.transform_));
        return kGfxResult_NoError;
    }

    GfxResult setRaytracingPrimitiveInstanceID(GfxRaytracingPrimitive const &raytracing_primitive, uint32_t instance_id)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set instanceID on an invalid raytracing primitive object");
        if(instance_id >= (1u << 24))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set an instanceID that is greater than %u", (1u << 24) - 1);
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        GFX_TRY(updateRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive));
        gfx_raytracing_primitive.instance_id_ = instance_id;
        return kGfxResult_NoError;
    }

    GfxResult setRaytracingPrimitiveInstanceMask(GfxRaytracingPrimitive const &raytracing_primitive, uint8_t instance_mask)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set instance mask on an invalid raytracing primitive object");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        GFX_TRY(updateRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive));
        gfx_raytracing_primitive.instance_mask_ = instance_mask;
        return kGfxResult_NoError;
    }

    GfxResult setRaytracingPrimitiveInstanceContributionToHitGroupIndex(GfxRaytracingPrimitive const &raytracing_primitive, uint32_t instance_contribution_to_hit_group_index)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set instance contribution to hit group index on an invalid raytracing primitive object");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        GFX_TRY(updateRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive));
        gfx_raytracing_primitive.instance_contribution_to_hit_group_index_ = instance_contribution_to_hit_group_index;
        return kGfxResult_NoError;
    }

    uint64_t getRaytracingPrimitiveDataSize(GfxRaytracingPrimitive const &raytracing_primitive)
    {
        if(dxr_device_ == nullptr) return 0;    // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot get the data size of an invalid raytracing primitive object");
            return 0;
        }
        RaytracingPrimitive const &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        switch(gfx_raytracing_primitive.type_)
        {
        case RaytracingPrimitive::kType_Triangles:
            return gfx_raytracing_primitive.triangles_.bvh_data_size_;
        case RaytracingPrimitive::kType_Procedural:
            return gfx_raytracing_primitive.procedural_.bvh_data_size_;
        default:
            break;
        }
        return 0;   // instanced raytracing primitives do not consume BVH memory
    }

    GfxResult updateRaytracingPrimitive(GfxRaytracingPrimitive const &raytracing_primitive)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot update an invalid raytracing primitive object");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        if(gfx_raytracing_primitive.type_ != RaytracingPrimitive::kType_Triangles)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a non-triangle raytracing primitive object");
        return buildRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive, true);
    }

    GfxResult updateRaytracingPrimitive(GfxRaytracingPrimitive const &raytracing_primitive, GfxBuffer const &vertex_buffer, uint32_t vertex_stride)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set geometry on an invalid raytracing primitive object");
        if(!buffer_handles_.has_handle(vertex_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot update a raytracing primitive using an invalid vertex buffer object");
        vertex_stride = (vertex_stride != 0 ? vertex_stride : vertex_buffer.stride);
        if(vertex_stride == 0)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive with a vertex buffer object of stride `0'");
        if(vertex_buffer.size / vertex_stride > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build a raytracing primitive with a buffer object containing more than 4 billion vertices");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        if(gfx_raytracing_primitive.type_ != RaytracingPrimitive::kType_Triangles)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a non-triangle raytracing primitive object");
        destroyBuffer(gfx_raytracing_primitive.triangles_.index_buffer_);
        destroyBuffer(gfx_raytracing_primitive.triangles_.vertex_buffer_);
        gfx_raytracing_primitive.triangles_.index_buffer_ = {};
        gfx_raytracing_primitive.triangles_.index_stride_ = 0;
        gfx_raytracing_primitive.triangles_.vertex_buffer_ = createBufferRange(vertex_buffer, 0, vertex_buffer.size);
        gfx_raytracing_primitive.triangles_.vertex_stride_ = vertex_stride;
        return buildRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive, true);
    }

    GfxResult updateRaytracingPrimitive(GfxRaytracingPrimitive const &raytracing_primitive, GfxBuffer const &index_buffer, GfxBuffer const &vertex_buffer, uint32_t vertex_stride)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set geometry on an invalid raytracing primitive object");
        if(!buffer_handles_.has_handle(index_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot update a raytracing primitive using an invalid index buffer object");
        if(!buffer_handles_.has_handle(vertex_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot update a raytracing primitive using an invalid vertex buffer object");
        uint32_t const index_stride = (index_buffer.stride == 2 ? 2 : 4);
        if(index_buffer.size / index_stride > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive with a buffer object containing more than 4 billion indices");
        vertex_stride = (vertex_stride != 0 ? vertex_stride : vertex_buffer.stride);
        if(vertex_stride == 0)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive with a vertex buffer object of stride `0'");
        if(vertex_buffer.size / vertex_stride > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive with a buffer object containing more than 4 billion vertices");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        if(gfx_raytracing_primitive.type_ != RaytracingPrimitive::kType_Triangles)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a non-triangle raytracing primitive object");
        destroyBuffer(gfx_raytracing_primitive.triangles_.index_buffer_);
        destroyBuffer(gfx_raytracing_primitive.triangles_.vertex_buffer_);
        gfx_raytracing_primitive.triangles_.index_buffer_ = createBufferRange(index_buffer, 0, index_buffer.size);
        gfx_raytracing_primitive.triangles_.index_stride_ = index_stride;
        gfx_raytracing_primitive.triangles_.vertex_buffer_ = createBufferRange(vertex_buffer, 0, vertex_buffer.size);
        gfx_raytracing_primitive.triangles_.vertex_stride_ = vertex_stride;
        return buildRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive, true);
    }

    GfxResult updateRaytracingPrimitiveProcedural(GfxRaytracingPrimitive const &raytracing_primitive, GfxBuffer const &aabb_buffer, uint32_t aabb_stride)
    {
        if(dxr_device_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set geometry on an invalid raytracing primitive object");
        if(!buffer_handles_.has_handle(aabb_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot update a raytracing primitive using an invalid AABB buffer object");
        aabb_stride = (aabb_stride != 0 ? aabb_stride : aabb_buffer.stride);
        if(aabb_stride == 0)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive with an AABB buffer object of stride `0'");
        if(aabb_buffer.size / aabb_stride > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive with a buffer object containing more than 4 billion AABBs");
        RaytracingPrimitive &gfx_raytracing_primitive = raytracing_primitives_[raytracing_primitive];
        if(gfx_raytracing_primitive.type_ != RaytracingPrimitive::kType_Procedural)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a non-procedural raytracing primitive object");
        destroyBuffer(gfx_raytracing_primitive.procedural_.procedural_buffer_);
        gfx_raytracing_primitive.procedural_.procedural_buffer_ = createBufferRange(aabb_buffer, 0, aabb_buffer.size);
        gfx_raytracing_primitive.procedural_.procedural_stride_ = aabb_stride;
        return buildRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive, true);
    }

    GfxProgram createProgram(char const *file_name, char const *file_path, char const *shader_model, char const **include_paths, uint32_t include_path_count)
    {
        GfxProgram program = {};
        if(!file_name)
            return program; // invalid parameter
        file_path = (file_path != nullptr ? file_path : ".");
        char const last_char = file_path[strlen(file_path) - 1];
        char const *path_separator = (last_char == '/' || last_char == '\\' ? "" : "/");
        GFX_SNPRINTF(program.name, sizeof(program.name), "%s%s%s", file_path, path_separator, file_name);
        shader_model = (shader_model != nullptr ? shader_model : dxr_device_ != nullptr ? "6_8" : "6_0");
        program.handle = program_handles_.allocate_handle();
        Program &gfx_program = programs_.insert(program);
        gfx_program.shader_model_ = shader_model;
        gfx_program.file_name_ = file_name;
        std::string const absolute_path = absolute(std::filesystem::path(file_path)).string();
        gfx_program.file_path_ = absolute_path.c_str();
        for(uint32_t i = 0; i < include_path_count; ++i) gfx_program.include_paths_.push_back(include_paths[i]);
        return program;
    }

    GfxProgram createProgram(GfxProgramDesc const &program_desc, char const *name, char const *shader_model, char const **include_paths, uint32_t include_path_count)
    {
        GfxProgram program = {};
        program.handle = program_handles_.allocate_handle();
        if(name != nullptr)
            GFX_SNPRINTF(program.name, sizeof(program.name), "%s", name);
        else
            GFX_SNPRINTF(program.name, sizeof(program.name), "gfx_Program%llu", program.handle);
        shader_model = (shader_model != nullptr ? shader_model : dxr_device_ != nullptr ? "6_8" : "6_0");
        Program &gfx_program = programs_.insert(program);
        gfx_program.shader_model_ = shader_model;
        gfx_program.file_path_ = (name != nullptr ? name : program.name);
        gfx_program.cs_ = program_desc.cs;
        gfx_program.as_ = program_desc.as;
        gfx_program.ms_ = program_desc.ms;
        gfx_program.vs_ = program_desc.vs;
        gfx_program.gs_ = program_desc.gs;
        gfx_program.ps_ = program_desc.ps;
        gfx_program.lib_ = program_desc.lib;
        for(uint32_t i = 0; i < include_path_count; ++i) gfx_program.include_paths_.push_back(include_paths[i]);
        return program;
    }

    GfxResult destroyProgram(GfxProgram const &program)
    {
        if(!program.handle)
            return kGfxResult_NoError;
        if(!program_handles_.has_handle(program.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot destroy invalid program object");
        collect(programs_[program]);    // release resources
        programs_.erase(program);   // destroy program
        program_handles_.free_handle(program.handle);
        return kGfxResult_NoError;
    }

    GfxResult setProgramBuffer(GfxProgram const &program, char const *parameter_name, GfxBuffer const &buffer)
    {
        if(!program_handles_.has_handle(program.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid program object");
        if(!parameter_name || !*parameter_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter with an invalid name");
        Program &gfx_program = programs_[program];  // get hold of program object
        gfx_program.insertParameter(parameter_name).set(&buffer, 1);
        return kGfxResult_NoError;
    }

    GfxResult setProgramBuffers(GfxProgram const &program, char const *parameter_name, GfxBuffer const *buffers, uint32_t buffer_count)
    {
        if(!program_handles_.has_handle(program.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid program object");
        if(!parameter_name || !*parameter_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter with an invalid name");
        if(buffers == nullptr && buffer_count > 0)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter to an invalid array of textures");
        Program &gfx_program = programs_[program];  // get hold of program object
        gfx_program.insertParameter(parameter_name).set(buffers, buffer_count);
        return kGfxResult_NoError;
    }

    GfxResult setProgramTexture(GfxProgram const &program, char const *parameter_name, GfxTexture const &texture, uint32_t mip_level)
    {
        if(!program_handles_.has_handle(program.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid program object");
        if(!parameter_name || !*parameter_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter with an invalid name");
        Program &gfx_program = programs_[program];  // get hold of program object
        gfx_program.insertParameter(parameter_name).set(&texture, &mip_level, 1);
        return kGfxResult_NoError;
    }

    GfxResult setProgramTextures(GfxProgram const &program, char const *parameter_name, GfxTexture const *textures, uint32_t const *mip_levels, uint32_t texture_count)
    {
        if(!program_handles_.has_handle(program.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid program object");
        if(!parameter_name || !*parameter_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter with an invalid name");
        if(textures == nullptr && texture_count > 0)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter to an invalid array of textures");
        Program &gfx_program = programs_[program];  // get hold of program object
        gfx_program.insertParameter(parameter_name).set(textures, mip_levels, texture_count);
        return kGfxResult_NoError;
    }

    GfxResult setProgramSamplerState(GfxProgram const &program, char const *parameter_name, GfxSamplerState const &sampler_state)
    {
        if(!program_handles_.has_handle(program.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid program object");
        if(!parameter_name || !*parameter_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter with an invalid name");
        Program &gfx_program = programs_[program];  // get hold of program object
        gfx_program.insertParameter(parameter_name).set(sampler_state);
        return kGfxResult_NoError;
    }

    GfxResult setProgramAccelerationStructure(GfxProgram const &program, char const *parameter_name, GfxAccelerationStructure const &acceleration_structure)
    {
        if(!program_handles_.has_handle(program.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid program object");
        if(!parameter_name || !*parameter_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter with an invalid name");
        Program &gfx_program = programs_[program];  // get hold of program object
        gfx_program.insertParameter(parameter_name).set(acceleration_structure);
        return kGfxResult_NoError;
    }

    GfxResult setProgramConstants(GfxProgram const &program, char const *parameter_name, void const *data, uint32_t data_size)
    {
        if(!program_handles_.has_handle(program.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid program object");
        if(!parameter_name || !*parameter_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter with an invalid name");
        if(!data && data_size > 0)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter to a null pointer");
        Program &gfx_program = programs_[program];  // get hold of program object
        gfx_program.insertParameter(parameter_name).set(data, data_size);
        return kGfxResult_NoError;
    }

    GfxKernel createMeshKernel(GfxProgram const &program, GfxDrawState const &draw_state, char const *entry_point, char const **defines, uint32_t define_count)
    {
        GfxKernel mesh_kernel = {};
        if(mesh_device_ == nullptr)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Mesh shaders aren't supported on the selected device; cannot create mesh kernel");
            return mesh_kernel; // invalid operation
        }
        if(!program_handles_.has_handle(program.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a mesh kernel using an invalid program object");
            return mesh_kernel;
        }
        GFX_ASSERT(define_count == 0 || defines != nullptr);
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState const *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a mesh kernel using an invalid draw state object");
            return mesh_kernel;
        }
        mesh_kernel.type = GfxKernel::kType_Mesh;
        Program const &gfx_program = programs_[program];
        entry_point = (entry_point ? entry_point : "main");
        GFX_SNPRINTF(mesh_kernel.name, sizeof(mesh_kernel.name), "%s", entry_point);
        mesh_kernel.handle = kernel_handles_.allocate_handle();
        Kernel &gfx_kernel = kernels_.insert(mesh_kernel);
        gfx_kernel.program_ = program;
        gfx_kernel.entry_point_ = entry_point;
        gfx_kernel.type_ = Kernel::kType_Mesh;
        gfx_kernel.draw_state_ = gfx_draw_state->draw_state_;
        for(uint32_t i = 0; i < define_count; ++i) gfx_kernel.defines_.push_back(defines[i]);
        gfx_kernel.num_threads_ = (uint32_t *)gfxMalloc(3 * sizeof(uint32_t)); for(uint32_t i = 0; i < 3; ++i) gfx_kernel.num_threads_[i] = 0;
        createKernel(gfx_program, gfx_kernel);  // create mesh kernel
        if(!gfx_program.file_name_ && (gfx_kernel.root_signature_ == nullptr || gfx_kernel.pipeline_state_ == nullptr))
        {
            destroyKernel(mesh_kernel);
            mesh_kernel = {};   // invalidate handle
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Failed to create mesh kernel object `%s' using program `%s'", entry_point, gfx_program.file_path_.c_str());
            return mesh_kernel;
        }
        return mesh_kernel;
    }

    GfxKernel createComputeKernel(GfxProgram const &program, char const *entry_point, char const **defines, uint32_t define_count)
    {
        GfxKernel compute_kernel = {};
        if(!program_handles_.has_handle(program.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a compute kernel using an invalid program object");
            return compute_kernel;
        }
        compute_kernel.type = GfxKernel::kType_Compute;
        Program const &gfx_program = programs_[program];
        entry_point = (entry_point ? entry_point : "main");
        GFX_SNPRINTF(compute_kernel.name, sizeof(compute_kernel.name), "%s", entry_point);
        compute_kernel.handle = kernel_handles_.allocate_handle();
        Kernel &gfx_kernel = kernels_.insert(compute_kernel);
        gfx_kernel.program_ = program;
        gfx_kernel.entry_point_ = entry_point;
        gfx_kernel.type_ = Kernel::kType_Compute;
        GFX_ASSERT(define_count == 0 || defines != nullptr);
        for(uint32_t i = 0; i < define_count; ++i) gfx_kernel.defines_.push_back(defines[i]);
        gfx_kernel.num_threads_ = (uint32_t *)gfxMalloc(3 * sizeof(uint32_t)); for(uint32_t i = 0; i < 3; ++i) gfx_kernel.num_threads_[i] = 1;
        createKernel(gfx_program, gfx_kernel);  // create compute kernel
        if(!gfx_program.file_name_ && (gfx_kernel.root_signature_ == nullptr || gfx_kernel.pipeline_state_ == nullptr))
        {
            destroyKernel(compute_kernel);
            compute_kernel = {};    // invalidate handle
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Failed to create compute kernel object `%s' using program `%s'", entry_point, gfx_program.file_path_.c_str());
            return compute_kernel;
        }
        return compute_kernel;
    }

    GfxKernel createGraphicsKernel(GfxProgram const &program, GfxDrawState const &draw_state, char const *entry_point, char const **defines, uint32_t define_count)
    {
        GfxResult result;
        GfxKernel graphics_kernel = {};
        if(!program_handles_.has_handle(program.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a graphics kernel using an invalid program object");
            return graphics_kernel;
        }
        GFX_ASSERT(define_count == 0 || defines != nullptr);
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState const *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a graphics kernel using an invalid draw state object");
            return graphics_kernel;
        }
        Program const &gfx_program = programs_[program];
        graphics_kernel.type = GfxKernel::kType_Graphics;
        entry_point = (entry_point ? entry_point : "main");
        GFX_SNPRINTF(graphics_kernel.name, sizeof(graphics_kernel.name), "%s", entry_point);
        graphics_kernel.handle = kernel_handles_.allocate_handle();
        Kernel &gfx_kernel = kernels_.insert(graphics_kernel);
        gfx_kernel.program_ = program;
        gfx_kernel.entry_point_ = entry_point;
        gfx_kernel.type_ = Kernel::kType_Graphics;
        gfx_kernel.draw_state_ = gfx_draw_state->draw_state_;
        for(uint32_t i = 0; i < define_count; ++i) gfx_kernel.defines_.push_back(defines[i]);
        gfx_kernel.num_threads_ = (uint32_t *)gfxMalloc(3 * sizeof(uint32_t)); for(uint32_t i = 0; i < 3; ++i) gfx_kernel.num_threads_[i] = 0;
        result = createKernel(gfx_program, gfx_kernel); // create graphics kernel
        if(result != kGfxResult_NoError)
        {
            destroyKernel(graphics_kernel);
            graphics_kernel = {};   // invalidate handle
            return graphics_kernel;
        }
        else if(!gfx_program.file_name_ && (gfx_kernel.root_signature_ == nullptr || gfx_kernel.pipeline_state_ == nullptr))
        {
            destroyKernel(graphics_kernel);
            graphics_kernel = {};   // invalidate handle
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Failed to create graphics kernel object `%s' using program `%s'", entry_point, gfx_program.file_path_.c_str());
            return graphics_kernel;
        }
        return graphics_kernel;
    }

    GfxKernel createRaytracingKernel(GfxProgram const &program,
        GfxLocalRootSignatureAssociation const *local_root_signature_associations, uint32_t local_root_signature_association_count,
        char const **exports, uint32_t export_count,
        char const **subobjects, uint32_t subobject_count,
        char const **defines, uint32_t define_count)
    {
        GfxKernel raytracing_kernel = {};
        if(dxr_device_ == nullptr)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Raytracing isn't supported on the selected device; cannot create raytracing kernel");
            return raytracing_kernel;   // invalid operation
        }
        if(!program_handles_.has_handle(program.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a compute kernel using an invalid program object");
            return raytracing_kernel;
        }
        raytracing_kernel.type = GfxKernel::kType_Raytracing;
        Program const &gfx_program = programs_[program];
        GFX_SNPRINTF(raytracing_kernel.name, sizeof(raytracing_kernel.name), "%s", "");
        raytracing_kernel.handle = kernel_handles_.allocate_handle();
        Kernel &gfx_kernel = kernels_.insert(raytracing_kernel);
        gfx_kernel.program_ = program;
        gfx_kernel.entry_point_ = "";
        gfx_kernel.type_ = Kernel::kType_Raytracing;
        GFX_ASSERT(define_count == 0 || defines != nullptr);
        GFX_ASSERT(local_root_signature_association_count == 0 || local_root_signature_associations != nullptr);
        for(uint32_t i = 0; i < define_count; ++i) gfx_kernel.defines_.push_back(defines[i]);
        for(uint32_t i = 0; i < export_count; ++i) gfx_kernel.exports_.push_back(exports[i]);
        for(uint32_t i = 0; i < subobject_count; ++i) gfx_kernel.subobjects_.push_back(subobjects[i]);
        std::wstring wgroup_name;
        for(uint32_t i = 0; i < local_root_signature_association_count; ++i)
        {
            wgroup_name.resize(mbstowcs(nullptr, local_root_signature_associations[i].shader_group_name, 0) + 1);
            mbstowcs(wgroup_name.data(), local_root_signature_associations[i].shader_group_name, wgroup_name.size());
            gfx_kernel.local_root_signature_associations_[wgroup_name] = { local_root_signature_associations[i].local_root_signature_space, local_root_signature_associations[i].shader_group_type };
        }
        gfx_kernel.num_threads_ = (uint32_t *)gfxMalloc(3 * sizeof(uint32_t)); for(uint32_t i = 0; i < 3; ++i) gfx_kernel.num_threads_[i] = 0;
        for(uint32_t i = 0; i < kGfxShaderGroupType_Count; ++i) gfx_kernel.sbt_record_stride_[i] = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        createKernel(gfx_program, gfx_kernel);  // create raytracing kernel
        if(!gfx_program.file_name_ && (gfx_kernel.root_signature_ == nullptr || gfx_kernel.pipeline_state_ == nullptr))
        {
            destroyKernel(raytracing_kernel);
            raytracing_kernel = {};    // invalidate handle
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Failed to create raytracing kernel object using program `%s'", gfx_program.file_path_.c_str());
            return raytracing_kernel;
        }
        return raytracing_kernel;
    }

    GfxResult destroyKernel(GfxKernel const &kernel)
    {
        if(!kernel)
            return kGfxResult_NoError;
        if(!kernel_handles_.has_handle(kernel.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot destroy invalid kernel object");
        collect(kernels_[kernel]);  // release resources
        kernels_.erase(kernel); // destroy kernel
        kernel_handles_.free_handle(kernel.handle);
        return kGfxResult_NoError;
    }

    static uint32_t const kNumThreads_Invalid[3];

    uint32_t const *getKernelNumThreads(GfxKernel const &kernel)
    {
        if(!kernel_handles_.has_handle(kernel.handle))
            return kNumThreads_Invalid; // invalid kernel object
        Kernel const &gfx_kernel = kernels_[kernel];
        GFX_ASSERT(gfx_kernel.num_threads_ != nullptr);
        return gfx_kernel.num_threads_;
    }

    GfxResult reloadAllKernels()
    {
        for(uint32_t i = 0; i < kernels_.size(); ++i)
            reloadKernel(kernels_.data()[i]);
        return kGfxResult_NoError;
    }

    GfxSbt createSbt(GfxKernel const *kernels, uint32_t kernel_count, uint32_t entry_count[kGfxShaderGroupType_Count])
    {
        GfxSbt sbt = {};
        if(dxr_device_ == nullptr)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Raytracing isn't supported on the selected device; cannot create SBT");
            return sbt; // invalid operation
        }
        sbt.handle = sbt_handles_.allocate_handle();
        Sbt &gfx_sbt = sbts_.insert(sbt);
        for(uint32_t i = 0; i < kernel_count; ++i)
        {
            Kernel const &gfx_kernel = kernels_[kernels[i]];
            for(uint32_t j = 0; j < kGfxShaderGroupType_Count; ++j)
            {
                gfx_sbt.sbt_max_record_stride_[j] = GFX_MAX(gfx_sbt.sbt_max_record_stride_[j], gfx_kernel.sbt_record_stride_[j]);
            }
        }
        for(uint32_t i = 0; i < kGfxShaderGroupType_Count; ++i)
        {
            size_t sbt_buffer_size = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - 1 + gfx_sbt.sbt_max_record_stride_[i] * entry_count[i];
            gfx_sbt.sbt_buffers_[i] = createBuffer(sbt_buffer_size, nullptr, kGfxCpuAccess_None);
            if(!gfx_sbt.sbt_buffers_[i])
            {
                GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to allocate memory for shader binding table");
                return sbt;
            }
            gfx_sbt.sbt_buffers_[i].setName("gfx_SbtBuffer");
        }
        gfx_sbt.ray_generation_shader_record_ = gfx_sbt.sbt_buffers_[kGfxShaderGroupType_Raygen] ?
            D3D12_GPU_VIRTUAL_ADDRESS_RANGE{
            GFX_ALIGN(buffers_[gfx_sbt.sbt_buffers_[kGfxShaderGroupType_Raygen]].resource_->GetGPUVirtualAddress(),
            D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT),
            gfx_sbt.sbt_max_record_stride_[kGfxShaderGroupType_Raygen]} :
            D3D12_GPU_VIRTUAL_ADDRESS_RANGE{};
        gfx_sbt.miss_shader_table_ = gfx_sbt.sbt_buffers_[kGfxShaderGroupType_Miss] ?
            D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{
            GFX_ALIGN(buffers_[gfx_sbt.sbt_buffers_[kGfxShaderGroupType_Miss]].resource_->GetGPUVirtualAddress(),
            D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT),
            entry_count[kGfxShaderGroupType_Miss] * gfx_sbt.sbt_max_record_stride_[kGfxShaderGroupType_Miss],
            gfx_sbt.sbt_max_record_stride_[kGfxShaderGroupType_Miss]} :
            D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{};
        gfx_sbt.hit_group_table_ = gfx_sbt.sbt_buffers_[kGfxShaderGroupType_Hit] ?
            D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{
            GFX_ALIGN(buffers_[gfx_sbt.sbt_buffers_[kGfxShaderGroupType_Hit]].resource_->GetGPUVirtualAddress(),
            D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT),
            entry_count[kGfxShaderGroupType_Hit] * gfx_sbt.sbt_max_record_stride_[kGfxShaderGroupType_Hit],
            gfx_sbt.sbt_max_record_stride_[kGfxShaderGroupType_Hit]} :
            D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{};
        gfx_sbt.callable_shader_table_ = gfx_sbt.sbt_buffers_[kGfxShaderGroupType_Callable] ?
            D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{
            GFX_ALIGN(buffers_[gfx_sbt.sbt_buffers_[kGfxShaderGroupType_Callable]].resource_->GetGPUVirtualAddress(),
            D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT),
            entry_count[kGfxShaderGroupType_Callable] * gfx_sbt.sbt_max_record_stride_[kGfxShaderGroupType_Callable],
            gfx_sbt.sbt_max_record_stride_[kGfxShaderGroupType_Callable]} :
            D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{};
        return sbt;
    }

    GfxResult destroySbt(GfxSbt const &sbt)
    {
        if(!sbt)
            return kGfxResult_NoError;
        if(!sbt_handles_.has_handle(sbt.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot destroy invalid sbt object");
        Sbt const &gfx_sbt = sbts_[sbt];
        collect(gfx_sbt);  // release resources
        sbts_.erase(sbt); // destroy sbt
        sbt_handles_.free_handle(sbt.handle);
        return kGfxResult_NoError;
    }

    GfxResult sbtSetShaderGroup(GfxSbt const &sbt, GfxShaderGroupType shader_group_type, uint32_t index, char const *group_name)
    {
        if(!sbt_handles_.has_handle(sbt.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid sbt object");
        if(!group_name || !*group_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a shader group with an invalid name");
        Sbt &gfx_sbt = sbts_[sbt];  // get hold of sbt object
        std::vector<WCHAR> wgroup_name(mbstowcs(nullptr, group_name, 0) + 1);
        mbstowcs(wgroup_name.data(), group_name, wgroup_name.size());
        gfx_sbt.insertSbtRecordShaderIdentifier(shader_group_type, index, wgroup_name.data());
        return kGfxResult_NoError;
    }

    GfxResult sbtSetConstants(GfxSbt const &sbt, GfxShaderGroupType shader_group_type, uint32_t index, char const *parameter_name, void const *data, uint32_t data_size)
    {
        if(!sbt_handles_.has_handle(sbt.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid sbt object");
        if(!parameter_name || !*parameter_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter with an invalid name");
        if(!data && data_size > 0)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter to a null pointer");
        Sbt &gfx_sbt = sbts_[sbt];  // get hold of sbt object
        auto record_and_param = gfx_sbt.insertSbtRecordParameter(shader_group_type, index, parameter_name);
        uint32_t const parameter_prev_id = record_and_param.second.id_;
        record_and_param.second.set(data, data_size);
        record_and_param.first.id_ += parameter_prev_id != record_and_param.second.id_;
        return kGfxResult_NoError;
    }

    GfxResult sbtSetTexture(GfxSbt const &sbt, GfxShaderGroupType shader_group_type, uint32_t index, char const *parameter_name, GfxTexture texture, uint32_t mip_level)
    {
        if(!sbt_handles_.has_handle(sbt.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set a parameter onto an invalid sbt object");
        if(!parameter_name || !*parameter_name)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set a program parameter with an invalid name");
        Sbt &gfx_sbt = sbts_[sbt];  // get hold of sbt object
        auto record_and_param = gfx_sbt.insertSbtRecordParameter(shader_group_type, index, parameter_name);
        uint32_t const parameter_prev_id = record_and_param.second.id_;
        record_and_param.second.set(&texture, &mip_level, 1);
        record_and_param.first.id_ += parameter_prev_id != record_and_param.second.id_;
        return kGfxResult_NoError;
    }

    GfxResult sbtGetGpuVirtualAddressRangeAndStride(GfxSbt const &sbt,
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE *ray_generation_shader_record,
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE *miss_shader_table,
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE *hit_group_table,
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE *callable_shader_table)
    {
        Sbt &gfx_sbt = sbts_[sbt];
        *ray_generation_shader_record = gfx_sbt.ray_generation_shader_record_;
        *miss_shader_table = gfx_sbt.miss_shader_table_;
        *hit_group_table = gfx_sbt.hit_group_table_;
        *callable_shader_table = gfx_sbt.callable_shader_table_;
        return kGfxResult_NoError;
    }

    GfxResult encodeCopyBuffer(GfxBuffer const &dst, GfxBuffer const &src)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!buffer_handles_.has_handle(dst.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy into an invalid buffer object");
        if(!buffer_handles_.has_handle(src.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy from an invalid buffer object");
        if(dst.size != src.size)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy between buffer objects of different size");
        if(dst.cpu_access == kGfxCpuAccess_Write)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy to a buffer object with write CPU access");
        if(src.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy from a buffer object with read CPU access");
        if(dst.size == 0) return kGfxResult_NoError;    // nothing to copy
        Buffer &gfx_dst = buffers_[dst], &gfx_src = buffers_[src];
        SetObjectName(gfx_dst, dst.name); SetObjectName(gfx_src, src.name);
        if(gfx_dst.resource_ == gfx_src.resource_)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy between two buffer objects that are pointing at the same resource; use an intermediate buffer");
        bool transitions = false;
        if(dst.cpu_access == kGfxCpuAccess_None) transitions = transitionResource(gfx_dst, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit);
        if(src.cpu_access == kGfxCpuAccess_None) transitions |= transitionResource(gfx_src, D3D12_RESOURCE_STATE_COPY_SOURCE, kTransitionType_Implicit);
        if(transitions)
            submitPipelineBarriers();   // transition our resources if needed
        command_list_->CopyBufferRegion(gfx_dst.resource_, gfx_dst.data_offset_,
                                        gfx_src.resource_, gfx_src.data_offset_,
                                        dst.size);
        return kGfxResult_NoError;
    }

    GfxResult encodeCopyBuffer(GfxBuffer const &dst, uint64_t dst_offset, GfxBuffer const &src, uint64_t src_offset, uint64_t size)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!buffer_handles_.has_handle(dst.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy into an invalid buffer object");
        if(!buffer_handles_.has_handle(src.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy from an invalid buffer object");
        if(dst_offset + size > dst.size || src_offset + size > src.size)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy between regions that are not fully contained within their respective buffer objects");
        if(dst.cpu_access == kGfxCpuAccess_Write)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy to a buffer object with write CPU access");
        if(src.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy from a buffer object with read CPU access");
        if(size == 0) return kGfxResult_NoError;    // nothing to copy
        Buffer &gfx_dst = buffers_[dst], &gfx_src = buffers_[src];
        SetObjectName(gfx_dst, dst.name); SetObjectName(gfx_src, src.name);
        if(gfx_dst.resource_ == gfx_src.resource_)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy between two buffer objects that are pointing at the same resource; use an intermediate buffer");
        bool transitions = false;
        if(dst.cpu_access == kGfxCpuAccess_None) transitions = transitionResource(gfx_dst, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit);
        if(src.cpu_access == kGfxCpuAccess_None) transitions |= transitionResource(gfx_src, D3D12_RESOURCE_STATE_COPY_SOURCE, kTransitionType_Implicit);
        if(transitions)
            submitPipelineBarriers();   // transition our resources if needed
        command_list_->CopyBufferRegion(gfx_dst.resource_, gfx_dst.data_offset_ + dst_offset,
                                        gfx_src.resource_, gfx_src.data_offset_ + src_offset,
                                        size);
        return kGfxResult_NoError;
    }

    GfxResult encodeClearBuffer(GfxBuffer buffer, uint32_t clear_value)
    {
        GfxResult result = kGfxResult_NoError;
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!buffer_handles_.has_handle(buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot clear an invalid buffer object");
        if(buffer.size == 0) return kGfxResult_NoError; // nothing to clear
        uint64_t const data_size = GFX_ALIGN(buffer.size, 4);
        uint64_t const num_uints = data_size / sizeof(uint32_t);
        buffer.setStride(sizeof(uint32_t));
        switch(buffer.cpu_access)
        {
        case kGfxCpuAccess_Write:
            {
                Buffer &gfx_buffer = buffers_[buffer];
                SetObjectName(gfx_buffer, buffer.name);
                uint32_t *data = (uint32_t *)gfx_buffer.data_;
                for(uint64_t i = 0; i < num_uints; ++i) data[i] = clear_value;
            }
            break;
        case kGfxCpuAccess_Read:
            {
                void *data = nullptr;
                D3D12_GPU_VIRTUAL_ADDRESS const gpu_addr = allocateConstantMemory(data_size, data);
                if(data == nullptr)
                    return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to clear buffer object");
                if(!issued_clear_buffer_warning_)
                    GFX_PRINTLN("Warning: It is inefficient to clear a buffer object with read CPU access; prefer a copy instead");
                issued_clear_buffer_warning_ = true;    // we've now warned the user...
                for(uint64_t i = 0; i < num_uints; ++i) ((uint32_t *)data)[i] = clear_value;
                Buffer &dst_buffer = buffers_[buffer], &src_buffer = buffers_[constant_buffer_pool_[fence_index_]];
                SetObjectName(dst_buffer, buffer.name);
                command_list_->CopyBufferRegion(dst_buffer.resource_, dst_buffer.data_offset_,
                    src_buffer.resource_, gpu_addr - src_buffer.resource_->GetGPUVirtualAddress(), data_size);
            }
            break;
        default:    // kGfxCpuAccess_None
            {
                GfxKernel const bound_kernel = bound_kernel_;
                setProgramBuffer(clear_buffer_program_, "OutputBuffer", buffer);
                setProgramConstants(clear_buffer_program_, "ClearValue", &clear_value, sizeof(clear_value));
                uint32_t const group_size = *getKernelNumThreads(clear_buffer_kernel_);
                uint32_t const num_groups = (uint32_t)((num_uints + group_size - 1) / group_size);
                GFX_TRY(encodeBindKernel(clear_buffer_kernel_));
                result = encodeDispatch(num_groups, 1, 1);
                if(kernel_handles_.has_handle(bound_kernel.handle))
                    encodeBindKernel(bound_kernel);
                else
                    bound_kernel_ = bound_kernel;
            }
            break;
        }
        if(result != kGfxResult_NoError)
            return GFX_SET_ERROR(result, "Failed to clear buffer object");
        return kGfxResult_NoError;
    }

    GfxResult encodeClearBackBuffer()
    {
        if(isInterop())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot clear backbuffer when using an interop context");
        float const clear_color[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        D3D12_RECT
        rect        = {};
        rect.right  = window_width_;
        rect.bottom = window_height_;
        command_list_->ClearRenderTargetView(rtv_descriptors_.getCPUHandle(back_buffer_rtvs_[fence_index_]), clear_color, 1, &rect);
        return kGfxResult_NoError;
    }

    GfxResult encodeClearTexture(GfxTexture const &texture)
    {
        uint32_t depth = texture.depth;
        for(uint32_t i = 0; i < texture.mip_levels; ++i)
        {
            for(uint32_t j = 0; j < depth; ++j)
                GFX_TRY(encodeClearImage(texture, i, j));
            if(texture.is3D())
                depth = GFX_MAX(depth >> 1, 1u);
        }
        return kGfxResult_NoError;
    }

    GfxResult encodeCopyTexture(GfxTexture const &dst, GfxTexture const &src)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!texture_handles_.has_handle(dst.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy to an invalid texture object");
        if(!texture_handles_.has_handle(src.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy from an invalid texture object");
        if(dst.type != src.type)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy between texture objects of different types");
        Texture &dst_texture = textures_[dst]; SetObjectName(dst_texture, dst.name);
        Texture &src_texture = textures_[src]; SetObjectName(src_texture, src.name);
        if(dst_texture.resource_ == src_texture.resource_)
            return kGfxResult_NoError;  // nothing to be copied
        bool transition = transitionResource(dst_texture, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit);
        transition |= transitionResource(src_texture, D3D12_RESOURCE_STATE_COPY_SOURCE, kTransitionType_Implicit);
        submitPipelineBarriers();   // transition our resources if needed
        if(dst.mip_levels == src.mip_levels)
            command_list_->CopyResource(dst_texture.resource_, src_texture.resource_);
        else
        {
            for(uint32_t mip_level = 0; mip_level < min(dst.mip_levels, src.mip_levels); ++mip_level)
            {
                D3D12_TEXTURE_COPY_LOCATION dst_location = {};
                D3D12_TEXTURE_COPY_LOCATION src_location = {};
                dst_location.pResource                   = dst_texture.resource_;
                dst_location.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_location.SubresourceIndex            = mip_level; // copy to mip level
                src_location.pResource                   = src_texture.resource_;
                src_location.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src_location.SubresourceIndex            = mip_level;
                command_list_->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
            }
        }
        return kGfxResult_NoError;
    }

    GfxResult encodeClearImage(GfxTexture const &texture, uint32_t mip_level, uint32_t slice)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!texture_handles_.has_handle(texture.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot clear an invalid texture object");
        if(mip_level >= texture.mip_levels)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot clear non-existing mip level %u", mip_level);
        if(slice >= (texture.is3D() ? GFX_MAX(texture.depth >> mip_level, 1u) : texture.depth))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot clear non-existing slice %u", slice);
        Texture &gfx_texture = textures_[texture];
        SetObjectName(gfx_texture, texture.name);
        if(IsDepthStencilFormat(texture.format))
        {
            D3D12_CLEAR_FLAGS clear_flags = D3D12_CLEAR_FLAG_DEPTH;
            if(HasStencilBits(texture.format)) clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
            GFX_TRY(ensureTextureHasDepthStencilView(texture, gfx_texture, mip_level, slice));
            GFX_ASSERT(gfx_texture.dsv_descriptor_slots_[mip_level][slice] != 0xFFFFFFFFu);
            D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
            D3D12_RECT
            rect        = {};
            rect.right  = (LONG)resource_desc.Width;
            rect.bottom = (LONG)resource_desc.Height;
            transitionResource(gfx_texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            submitPipelineBarriers();   // transition our resources if needed
            command_list_->ClearDepthStencilView(dsv_descriptors_.getCPUHandle(gfx_texture.dsv_descriptor_slots_[mip_level][slice]), clear_flags, gfx_texture.clear_value_[0], (UINT8)gfx_texture.clear_value_[1], 1, &rect);
        }
        else
        {
            GFX_TRY(ensureTextureHasRenderTargetView(texture, gfx_texture, mip_level, slice));
            GFX_ASSERT(gfx_texture.rtv_descriptor_slots_[mip_level][slice] != 0xFFFFFFFFu);
            D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
            D3D12_RECT
            rect        = {};
            rect.right  = (LONG)resource_desc.Width;
            rect.bottom = (LONG)resource_desc.Height;
            transitionResource(gfx_texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
            submitPipelineBarriers();   // transition our resources if needed
            command_list_->ClearRenderTargetView(rtv_descriptors_.getCPUHandle(gfx_texture.rtv_descriptor_slots_[mip_level][slice]), gfx_texture.clear_value_, 1, &rect);
        }
        return kGfxResult_NoError;
    }

    GfxResult encodeCopyTextureToBackBuffer(GfxTexture const &texture)
    {
        GfxResult result;
        if(isInterop())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy to backbuffer when using an interop context");
        if(!texture_handles_.has_handle(texture.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy from an invalid texture object");
        if(!texture.is2D())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy from a non-2D texture object");
        Texture &gfx_texture = textures_[texture]; SetObjectName(gfx_texture, texture.name);
        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
        if(window_width_ != (uint32_t)resource_desc.Width || window_height_ != (uint32_t)resource_desc.Height)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy between texture objects that do not have the same dimensions");
        GfxKernel const bound_kernel = bound_kernel_;
        GFX_TRY(encodeBindKernel(copy_to_backbuffer_kernel_));
        setProgramTexture(copy_to_backbuffer_program_, "InputBuffer", texture, 0);
        result = encodeDraw(3, 1, 0, 0);    // copy to backbuffer
        if(kernel_handles_.has_handle(bound_kernel.handle))
            encodeBindKernel(bound_kernel);
        else
            bound_kernel_ = bound_kernel;
        if(result != kGfxResult_NoError)
            return GFX_SET_ERROR(result, "Failed to copy texture object to backbuffer");
        return kGfxResult_NoError;
    }

    GfxResult encodeCopyBufferToTexture(GfxTexture const &dst, GfxBuffer const &src)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!texture_handles_.has_handle(dst.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy to an invalid texture object");
        if(!buffer_handles_.has_handle(src.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy from an invalid buffer object");
        if(!dst.is2D()) // TODO: implement for the other texture types (gboisse)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy from buffer to a non-2D texture object");
        if(src.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy from a buffer object with read CPU access");
        Texture &gfx_texture = textures_[dst]; SetObjectName(gfx_texture, dst.name);
        uint32_t num_rows[D3D12_REQ_MIP_LEVELS] = {};
        uint64_t row_sizes[D3D12_REQ_MIP_LEVELS] = {};
        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT subresource_footprints[D3D12_REQ_MIP_LEVELS] = {};
        device_->GetCopyableFootprints(&resource_desc, 0, dst.mip_levels, 0, subresource_footprints, num_rows, row_sizes, nullptr);
        uint64_t buffer_offset = 0;
        uint32_t const bytes_per_pixel = GetBytesPerPixel(dst.format);
        if(bytes_per_pixel == 0)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy to texture object of unsupported format");
        Buffer &gfx_buffer = buffers_[src]; SetObjectName(gfx_buffer, src.name);
        bool transition = transitionResource(gfx_texture, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit);
        if(src.cpu_access == kGfxCpuAccess_None) transition |= transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, kTransitionType_Implicit);
        if(transition)
            submitPipelineBarriers();
        D3D12_FEATURE_DATA_D3D12_OPTIONS13 features = {};   // feature check
        device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &features, sizeof(features));
        bool unrestricted_pitch = features.UnrestrictedBufferTextureCopyPitchSupported;
        for(uint32_t mip_level = 0; mip_level < dst.mip_levels; ++mip_level)
        {
            if(buffer_offset >= src.size)
                break;  // further mips aren't available
            uint64_t const buffer_row_pitch = row_sizes[mip_level];
            uint64_t const buffer_size = (uint64_t)num_rows[mip_level] * buffer_row_pitch;
            if(buffer_offset + buffer_size > src.size)
                return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy to mip level %u from buffer object with insufficient storage", mip_level);
            Buffer *texture_upload_buffer = nullptr;
            uint64_t const texture_row_pitch = subresource_footprints[mip_level].Footprint.RowPitch;
            if(!unrestricted_pitch && (buffer_row_pitch != texture_row_pitch || // we must respect the 256-byte pitch alignment
               GFX_ALIGN(buffer_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) != buffer_offset))
            {
                uint64_t texture_upload_buffer_size = num_rows[mip_level] * static_cast<uint64_t>(subresource_footprints[mip_level].Footprint.RowPitch);
                if(texture_upload_buffer_.size < texture_upload_buffer_size)
                {
                    destroyBuffer(texture_upload_buffer_);
                    texture_upload_buffer_size += (texture_upload_buffer_size + 2) >> 1;
                    texture_upload_buffer_size = GFX_ALIGN(texture_upload_buffer_size, 65536);
                    texture_upload_buffer_ = createBuffer(texture_upload_buffer_size, nullptr, kGfxCpuAccess_None);
                    if(!texture_upload_buffer_)
                        return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to allocate memory to upload texture data");
                    texture_upload_buffer_.setName("gfx_TextureUploadBuffer");
                }
                texture_upload_buffer = &buffers_[texture_upload_buffer_];
                SetObjectName(*texture_upload_buffer, texture_upload_buffer_.name);
                if(transitionResource(*texture_upload_buffer, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit))
                    submitPipelineBarriers();   // transition our resources if needed
                for(uint32_t i = 0; i < num_rows[mip_level]; ++i)
                    command_list_->CopyBufferRegion(texture_upload_buffer->resource_, i * texture_row_pitch, gfx_buffer.resource_, i * buffer_row_pitch + buffer_offset, buffer_row_pitch);
                transitionResource(*texture_upload_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
                submitPipelineBarriers();
            }
            {
                D3D12_TEXTURE_COPY_LOCATION dst_location = {};
                D3D12_TEXTURE_COPY_LOCATION src_location = {};
                dst_location.pResource = gfx_texture.resource_;
                dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_location.SubresourceIndex = mip_level;  // copy to mip level
                src_location.pResource = (texture_upload_buffer == nullptr ? gfx_buffer.resource_ : texture_upload_buffer->resource_);
                src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src_location.PlacedFootprint.Footprint = subresource_footprints[mip_level].Footprint;
                if(texture_upload_buffer == nullptr)
                    src_location.PlacedFootprint.Footprint.RowPitch = (UINT)buffer_row_pitch;
                src_location.PlacedFootprint.Offset = (texture_upload_buffer == nullptr ? buffer_offset : 0);
                command_list_->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
            }
            buffer_offset += buffer_size;   // advance the buffer offset
        }
        return kGfxResult_NoError;
    }

    GfxResult encodeCopyBufferToCubeFace(GfxTexture const &dst, GfxBuffer const &src, uint32_t face)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!texture_handles_.has_handle(dst.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy to an invalid texture object");
        if(!buffer_handles_.has_handle(src.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy from an invalid buffer object");
        if(dst.type != GfxTexture::kType_Cube)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy from buffer to a non-cube texture object");
        if(src.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy from a buffer object with read CPU access");
        Texture &gfx_texture = textures_[dst];
        SetObjectName(gfx_texture, dst.name);
        uint32_t num_rows[D3D12_REQ_MIP_LEVELS] = {};
        uint64_t row_sizes[D3D12_REQ_MIP_LEVELS] = {};
        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT subresource_footprints[D3D12_REQ_MIP_LEVELS] = {};
        device_->GetCopyableFootprints(&resource_desc, face * dst.mip_levels, dst.mip_levels, 0, subresource_footprints, num_rows, row_sizes, nullptr);
        uint64_t buffer_offset = 0;
        uint32_t const bytes_per_pixel = GetBytesPerPixel(dst.format);
        if(bytes_per_pixel == 0)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy to texture object of unsupported format");
        Buffer &gfx_buffer = buffers_[src];
        SetObjectName(gfx_buffer, src.name);
        bool transition = transitionResource(gfx_texture, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit);
        if(src.cpu_access == kGfxCpuAccess_None)
            transition |= transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, kTransitionType_Implicit);
        if(transition)
            submitPipelineBarriers();
        for(uint32_t mip_level = 0; mip_level < dst.mip_levels; ++mip_level)
        {
            if(buffer_offset >= src.size)
                break;  // further mips aren't available
            uint32_t subresource_index = face * dst.mip_levels + mip_level;
            uint64_t const buffer_row_pitch = row_sizes[mip_level];
            uint64_t const buffer_size = (uint64_t)num_rows[mip_level] * buffer_row_pitch;
            if(buffer_offset + buffer_size > src.size)
                return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy to mip level %u from buffer object with insufficient storage", mip_level);
            D3D12_TEXTURE_COPY_LOCATION dst_location = {.pResource = gfx_texture.resource_, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = subresource_index};
            D3D12_TEXTURE_COPY_LOCATION src_location = {gfx_buffer.resource_, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {subresource_footprints[mip_level]}};
            command_list_->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
            buffer_offset += buffer_size;   // advance the buffer offset
        }
        return kGfxResult_NoError;
    }

    GfxResult encodeCopyTextureToBuffer(GfxBuffer const &dst, GfxTexture const &src)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!texture_handles_.has_handle(src.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy from an invalid texture object");
        if(!buffer_handles_.has_handle(dst.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot copy to an invalid buffer object");
        if(!src.is2D()) // TODO: implement for the other texture types (gboisse)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy a non-2D texture object");
        if(dst.cpu_access == kGfxCpuAccess_Write)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy to a buffer object with write CPU access");
        Texture &gfx_texture = textures_[src]; SetObjectName(gfx_texture, src.name);
        Buffer &gfx_buffer = buffers_[dst]; SetObjectName(gfx_buffer, dst.name);
        uint32_t num_rows[D3D12_REQ_MIP_LEVELS] = {};
        uint64_t row_sizes[D3D12_REQ_MIP_LEVELS] = {};
        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT subresource_footprints[D3D12_REQ_MIP_LEVELS] = {};
        device_->GetCopyableFootprints(&resource_desc, 0, src.mip_levels, 0, subresource_footprints, num_rows, row_sizes, nullptr);
        uint64_t buffer_offset = 0;
        uint32_t const bytes_per_pixel = GetBytesPerPixel(src.format);
        if(bytes_per_pixel == 0)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy from texture object of unsupported format");
        bool transition = transitionResource(gfx_texture, D3D12_RESOURCE_STATE_COPY_SOURCE, kTransitionType_Implicit);
        if(dst.cpu_access == kGfxCpuAccess_None) transition |= transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit);
        if(transition)
            submitPipelineBarriers();   // transition our resources if needed
        for(uint32_t mip_level = 0; mip_level < src.mip_levels; ++mip_level)
        {
            if(buffer_offset >= dst.size)
                break;  // further mips aren't available
            uint64_t const buffer_row_pitch = row_sizes[mip_level];
            uint64_t const buffer_size = (uint64_t)num_rows[mip_level] * buffer_row_pitch;
            if(buffer_offset + buffer_size > dst.size)
                return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot copy from mip level %u to buffer object with insufficient storage", mip_level);
            {
                D3D12_TEXTURE_COPY_LOCATION dst_location = {};
                D3D12_TEXTURE_COPY_LOCATION src_location = {};
                src_location.pResource = gfx_texture.resource_;
                src_location.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src_location.SubresourceIndex = mip_level; // copy from mip level
                dst_location.pResource = gfx_buffer.resource_;
                dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst_location.PlacedFootprint.Footprint = subresource_footprints[mip_level].Footprint;
                dst_location.PlacedFootprint.Footprint.RowPitch = (UINT)buffer_row_pitch;
                dst_location.PlacedFootprint.Offset = buffer_offset;
                command_list_->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
            }
            buffer_offset += buffer_size;   // advance the buffer offset
        }
        return kGfxResult_NoError;
    }

    GfxResult encodeGenerateMips(GfxTexture const &texture)
    {
        GfxResult result = kGfxResult_NoError;
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!texture_handles_.has_handle(texture.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot generate mips of an invalid texture object");
        if(!texture.is2D() && !texture.is2DArray() && !texture.isCube())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot generate mips of a 3D texture object");
        if(texture.mip_levels <= 1) return kGfxResult_NoError;  // nothing to generate
        GfxKernel const bound_kernel = bound_kernel_;
        MipKernels const &mip_kernels = getMipKernels(texture);
        GFX_TRY(encodeBindKernel(mip_kernels.mip_kernel_));
        bool const isSRGB = IsSRGBFormat(texture.format);
        uint32_t elements = (texture.is2D()) ? 1 : texture.getDepth();
        uint32_t const *num_threads = getKernelNumThreads(mip_kernels.mip_kernel_);
        uint32_t const num_groups_z = (elements + num_threads[2] - 1) / num_threads[2];
        for(uint32_t mip_level = 1; mip_level < texture.mip_levels; ++mip_level)
        {
            setProgramTexture(mip_kernels.mip_program_, "InputBuffer", texture, mip_level - 1);
            setProgramTexture(mip_kernels.mip_program_, "OutputBuffer", texture, mip_level);
            setProgramConstants(mip_kernels.mip_program_, "isSRGB", &isSRGB, sizeof(isSRGB));
            uint32_t const num_groups_x = (GFX_MAX(texture.width >> mip_level, 1u) + num_threads[0] - 1) / num_threads[0];
            uint32_t const num_groups_y = (GFX_MAX(texture.height >> mip_level, 1u) + num_threads[1] - 1) / num_threads[1];
            result = encodeDispatch(num_groups_x, num_groups_y, num_groups_z);
            if(result != kGfxResult_NoError) break;
        }
        if(kernel_handles_.has_handle(bound_kernel.handle))
            encodeBindKernel(bound_kernel);
        else
            bound_kernel_ = bound_kernel;
        if(result != kGfxResult_NoError)
            return GFX_SET_ERROR(result, "Failed to generate texture mips");
        return kGfxResult_NoError;
    }

    GfxResult encodeBindColorTarget(uint32_t target_index, GfxTexture target_texture, uint32_t mip_level, uint32_t slice)
    {
        if(target_index >= kGfxConstant_MaxRenderTarget)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot bind more than %u render targets", (uint32_t)kGfxConstant_MaxRenderTarget);
        if(!target_texture)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to an invalid texture object");
        if(mip_level >= target_texture.mip_levels)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to mip level that does not exist in texture object");
        if(slice >= (target_texture.is3D() ? GFX_MAX(target_texture.depth >> mip_level, 1u) : target_texture.depth))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to slice that does not exist in texture object");
        bound_color_targets_[target_index].texture_  = target_texture;
        bound_color_targets_[target_index].mip_level_ = mip_level;
        bound_color_targets_[target_index].slice_     = slice;
        return kGfxResult_NoError;
    }

    GfxResult encodeBindDepthStencilTarget(GfxTexture target_texture, uint32_t mip_level, uint32_t slice)
    {
        if(!target_texture)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to an invalid texture object");
        if(mip_level >= target_texture.mip_levels)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to mip level that does not exist in texture object");
        if(slice >= (target_texture.is3D() ? GFX_MAX(target_texture.depth >> mip_level, 1u) : target_texture.depth))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to slice that does not exist in texture object");
        bound_depth_stencil_target_.texture_   = target_texture;
        bound_depth_stencil_target_.mip_level_ = mip_level;
        bound_depth_stencil_target_.slice_     = slice;
        return kGfxResult_NoError;
    }

    GfxResult encodeBindKernel(GfxKernel const &kernel)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if((kernel.isMesh() && mesh_command_list_ == nullptr))
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!kernel_handles_.has_handle(kernel.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot bind invalid kernel object");
        if(bound_kernel_.handle == kernel.handle) return kGfxResult_NoError;    // already bound
        Kernel const &gfx_kernel = kernels_[kernel];
        if(gfx_kernel.isRaytracing())
        {
            if(gfx_kernel.state_object_ != nullptr)
                dxr_command_list_->SetPipelineState1(gfx_kernel.state_object_);
        }
        else
        {
            if(gfx_kernel.pipeline_state_ != nullptr)
                command_list_->SetPipelineState(gfx_kernel.pipeline_state_);
        }
        if(gfx_kernel.root_signature_ != nullptr)
        {
            if(gfx_kernel.isCompute() || gfx_kernel.isRaytracing())
                command_list_->SetComputeRootSignature(gfx_kernel.root_signature_);
            else
                command_list_->SetGraphicsRootSignature(gfx_kernel.root_signature_);
        }
        bound_kernel_ = kernel; // bind kernel
        return kGfxResult_NoError;
    }

    GfxResult encodeBindIndexBuffer(GfxBuffer const &index_buffer)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!buffer_handles_.has_handle(index_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot bind index data from invalid buffer object");
        if(index_buffer.size > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot bind an index buffer object that's larger than 4GiB");
        if(index_buffer.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot bind an index buffer object that has read CPU access");
        bound_index_buffer_ = index_buffer;
        return kGfxResult_NoError;
    }

    GfxResult encodeBindVertexBuffer(GfxBuffer const &vertex_buffer)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!buffer_handles_.has_handle(vertex_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot bind vertex data from invalid buffer object");
        if(vertex_buffer.size > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot bind a vertex buffer object that's larger than 4GiB");
        if(vertex_buffer.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot bind a vertex buffer object that has read CPU access");
        bound_vertex_buffer_ = vertex_buffer;
        return kGfxResult_NoError;
    }

    GfxResult encodeSetViewport(float x, float y, float width, float height)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(isnan(x) || isnan(y) || isnan(width) || isnan(height))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set viewport using invalid floating-point numbers");
        viewport_.x_ = x;
        viewport_.y_ = y;
        viewport_.width_ = width;
        viewport_.height_ = height;
        return kGfxResult_NoError;
    }

    GfxResult encodeSetScissorRect(int32_t x, int32_t y, int32_t width, int32_t height)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(width < 0 || height < 0)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set scissor rect using negative width/height values");
        scissor_rect_.x_ = x;
        scissor_rect_.y_ = y;
        scissor_rect_.width_ = width;
        scissor_rect_.height_ = height;
        return kGfxResult_NoError;
    }

    GfxResult encodeDraw(uint32_t vertex_count, uint32_t instance_count, uint32_t base_vertex, uint32_t base_instance)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(vertex_count == 0 || instance_count == 0)
            return kGfxResult_NoError;  // nothing to draw
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isGraphics())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw using a non-graphics kernel object");
        if(kernel.root_signature_ == nullptr || kernel.pipeline_state_ == nullptr)
            return kGfxResult_NoError;  // skip draw call
        GFX_TRY(installShaderState(kernel));
        submitPipelineBarriers();   // transition our resources if needed
        command_list_->DrawInstanced(vertex_count, instance_count, base_vertex, base_instance);
        return kGfxResult_NoError;
    }

    GfxResult encodeDrawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, uint32_t base_vertex, uint32_t base_instance)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(index_count == 0 || instance_count == 0)
            return kGfxResult_NoError;  // nothing to draw
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isGraphics())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw using a non-graphics kernel object");
        if(kernel.root_signature_ == nullptr || kernel.pipeline_state_ == nullptr)
            return kGfxResult_NoError;  // skip draw call
        GFX_TRY(installShaderState(kernel, true));
        submitPipelineBarriers();   // transition our resources if needed
        command_list_->DrawIndexedInstanced(index_count, instance_count, first_index, base_vertex, base_instance);
        return kGfxResult_NoError;
    }

    GfxResult encodeMultiDrawIndirect(GfxBuffer const &args_buffer, uint32_t args_count)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(args_count == 0)
            return kGfxResult_NoError;  // nothing to draw
        if(!buffer_handles_.has_handle(args_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot draw using an invalid arguments buffer object");
        if(args_buffer.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw using an arguments buffer object with read CPU access");
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isGraphics())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw using a non-graphics kernel object");
        if(kernel.root_signature_ == nullptr || kernel.pipeline_state_ == nullptr)
            return kGfxResult_NoError;  // skip multi-draw call
        GFX_TRY(populateDrawIdBuffer(args_count));
        Buffer &gfx_buffer = buffers_[args_buffer];
        SetObjectName(gfx_buffer, args_buffer.name);
        GFX_TRY(installShaderState(kernel));
        if(args_buffer.cpu_access == kGfxCpuAccess_None)
            transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, kTransitionType_Implicit);
        submitPipelineBarriers();   // transition our resources if needed
        command_list_->ExecuteIndirect(multi_draw_signature_, args_count, gfx_buffer.resource_, gfx_buffer.data_offset_, nullptr, 0);
        return kGfxResult_NoError;
    }

    GfxResult encodeMultiDrawIndexedIndirect(GfxBuffer const &args_buffer, uint32_t args_count)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(args_count == 0)
            return kGfxResult_NoError;  // nothing to draw
        if(!buffer_handles_.has_handle(args_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot draw using an invalid arguments buffer object");
        if(args_buffer.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw using an arguments buffer object with read CPU access");
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isGraphics())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw using a non-graphics kernel object");
        if(kernel.root_signature_ == nullptr || kernel.pipeline_state_ == nullptr)
            return kGfxResult_NoError;  // skip multi-draw call
        GFX_TRY(populateDrawIdBuffer(args_count));
        Buffer &gfx_buffer = buffers_[args_buffer];
        SetObjectName(gfx_buffer, args_buffer.name);
        GFX_TRY(installShaderState(kernel, true));
        if(args_buffer.cpu_access == kGfxCpuAccess_None)
            transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, kTransitionType_Implicit);
        submitPipelineBarriers();   // transition our resources if needed
        command_list_->ExecuteIndirect(multi_draw_indexed_signature_, args_count, gfx_buffer.resource_, gfx_buffer.data_offset_, nullptr, 0);
        return kGfxResult_NoError;
    }

    GfxResult encodeDispatch(uint32_t num_groups_x, uint32_t num_groups_y, uint32_t num_groups_z)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!num_groups_x || !num_groups_y || !num_groups_z)
            return kGfxResult_NoError;  // nothing to dispatch
        uint32_t const max_num_groups = 65535;  // AMD doesn't allow to dispatch more than 65535 groups at once
        if(num_groups_x > max_num_groups || num_groups_y > max_num_groups || num_groups_z > max_num_groups)
        {
            GfxResult result;
            GfxBuffer args_buffer = allocateConstantMemory(3 * sizeof(uint32_t));
            uint32_t *args = (uint32_t *)getBufferData(args_buffer); GFX_ASSERT(args != nullptr);
            args[0] = num_groups_x; args[1] = num_groups_y; args[2] = num_groups_z;
            result = encodeDispatchIndirect(args_buffer);   // use indirect dispatch to workaround group limit
            destroyBuffer(args_buffer);
            return result;
        }
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isCompute())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch using a non-compute kernel object");
        if(kernel.root_signature_ == nullptr || kernel.pipeline_state_ == nullptr)
            return kGfxResult_NoError;  // skip dispatch call
        GFX_TRY(installShaderState(kernel));
        submitPipelineBarriers();   // transition our resources if needed
        command_list_->Dispatch(num_groups_x, num_groups_y, num_groups_z);
        return kGfxResult_NoError;
    }

    GfxResult encodeDispatchIndirect(GfxBuffer args_buffer)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!buffer_handles_.has_handle(args_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot dispatch using an invalid arguments buffer object");
        if(args_buffer.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch using an arguments buffer object with read CPU access");
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isCompute())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch using a non-compute kernel object");
        if(kernel.root_signature_ == nullptr || kernel.pipeline_state_ == nullptr)
            return kGfxResult_NoError;  // skip dispatch call
        Buffer &gfx_buffer = buffers_[args_buffer];
        SetObjectName(gfx_buffer, args_buffer.name);
        GFX_TRY(installShaderState(kernel));
        if(args_buffer.cpu_access == kGfxCpuAccess_None)
            transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, kTransitionType_Implicit);
        submitPipelineBarriers();   // transition our resources if needed
        command_list_->ExecuteIndirect(dispatch_signature_, 1, gfx_buffer.resource_, gfx_buffer.data_offset_, nullptr, 0);
        return kGfxResult_NoError;
    }

    GfxResult encodeMultiDispatchIndirect(GfxBuffer args_buffer, uint32_t args_count)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(args_count == 0)
            return kGfxResult_NoError;  // nothing to dispatch
        if(!buffer_handles_.has_handle(args_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot dispatch using an invalid arguments buffer object");
        if(args_buffer.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch using an arguments buffer object with read CPU access");
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isCompute())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch using a non-compute kernel object");
        if(kernel.root_signature_ == nullptr || kernel.pipeline_state_ == nullptr)
            return kGfxResult_NoError;  // skip multi-dispatch call
        Buffer &gfx_buffer = buffers_[args_buffer];
        SetObjectName(gfx_buffer, args_buffer.name);
        GFX_TRY(installShaderState(kernel));
        if(args_buffer.cpu_access == kGfxCpuAccess_None)
            transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, kTransitionType_Implicit);
        submitPipelineBarriers();   // transition our resources if needed
        uint32_t root_parameter_index = 0xFFFFFFFFu, destination_offset = 0;
        static uint64_t const dispatch_id_parameter = Hash("gfx_DispatchID");
        for(uint32_t i = 0; i < kernel.parameter_count_; ++i)
            if(kernel.parameters_[i].type_ == Kernel::Parameter::kType_Constants)
                for(uint32_t j = 0; j < kernel.parameters_[i].variable_count_; ++j)
                    if(kernel.parameters_[i].variables_[j].parameter_id_ == dispatch_id_parameter)
                    {
                        root_parameter_index = i;   // found root parameter
                        destination_offset = kernel.parameters_[i].variables_[j].data_start_ / sizeof(uint32_t);
                        i = kernel.parameter_count_;
                        break;  // located "gfx_DispatchID"
                    }
        if(root_parameter_index == 0xFFFFFFFFu)
        {
            static bool warned;
            if(!warned)
                GFX_PRINTLN("Warning: unable to locate `gfx_DispatchID' root constant for multi-dispatch call");
            warned = true;  // user was warned
        }
        for(uint32_t dispatch_id = 0; dispatch_id < args_count; ++dispatch_id)
        {
            if(dispatch_id > 0 && root_parameter_index != 0xFFFFFFFFu)  // <- patch root constants
                command_list_->SetComputeRoot32BitConstant(root_parameter_index, dispatch_id, destination_offset);
            command_list_->ExecuteIndirect(dispatch_signature_, 1, gfx_buffer.resource_, gfx_buffer.data_offset_ + dispatch_id * sizeof(D3D12_DISPATCH_ARGUMENTS), nullptr, 0);
        }
        return kGfxResult_NoError;
    }

    GfxResult encodeDispatchRays(GfxSbt const &sbt, uint32_t width, uint32_t height, uint32_t depth)
    {
        if(dxr_command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!sbt_handles_.has_handle(sbt.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot dispatch using an invalid sbt object");
        if(!width || !height || !depth)
            return kGfxResult_NoError;  // nothing to dispatch
        uint32_t const max_num_groups = 65535; // AMD doesn't allow to dispatch more than 65535 groups at once
        if(width > max_num_groups || height > max_num_groups || depth > max_num_groups)
        {
            GfxResult result;
            GfxBuffer args_buffer = allocateConstantMemory(3 * sizeof(uint32_t));
            uint32_t *args        = (uint32_t *)getBufferData(args_buffer);
            GFX_ASSERT(args != nullptr);
            args[0] = width;
            args[1] = height;
            args[2] = depth;
            result  = encodeDispatchRaysIndirect(sbt, args_buffer); // use indirect dispatch to workaround group limit
            destroyBuffer(args_buffer);
            return result;
        }
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isRaytracing())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch using a non-raytracing kernel object");
        if(kernel.root_signature_ == nullptr || kernel.state_object_ == nullptr)
            return kGfxResult_NoError;  // skip dispatch call
        Sbt &gfx_sbt = sbts_[sbt];
        GFX_TRY(installShaderState(kernel, false, &gfx_sbt));
        submitPipelineBarriers();   // transition our resources if needed
        D3D12_DISPATCH_RAYS_DESC desc;
        desc.RayGenerationShaderRecord = gfx_sbt.ray_generation_shader_record_;
        desc.MissShaderTable = gfx_sbt.miss_shader_table_;
        desc.HitGroupTable = gfx_sbt.hit_group_table_;
        desc.CallableShaderTable = gfx_sbt.callable_shader_table_;
        desc.Width = width;
        desc.Height = height;
        desc.Depth = depth;
        dxr_command_list_->DispatchRays(&desc);
        return kGfxResult_NoError;
    }

    GfxResult encodeDispatchRaysIndirect(GfxSbt const &sbt, GfxBuffer args_buffer)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(!sbt_handles_.has_handle(sbt.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot dispatch using an invalid sbt object");
        if(!buffer_handles_.has_handle(args_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot dispatch rays using an invalid arguments buffer object");
        if(args_buffer.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch rays using an arguments buffer object with read CPU access");
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch rays when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isRaytracing())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot dispatch rays using non-raytracing kernel object");
        if(kernel.root_signature_ == nullptr || kernel.state_object_ == nullptr)
            return kGfxResult_NoError;  // skip dispatch rays call
        Buffer &gfx_buffer = buffers_[args_buffer];
        SetObjectName(gfx_buffer, args_buffer.name);
        Sbt &gfx_sbt = sbts_[sbt]; // get hold of sbt object
        GFX_TRY(installShaderState(kernel, false, &gfx_sbt));
        if(args_buffer.cpu_access == kGfxCpuAccess_None)
            transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, kTransitionType_Implicit);
        submitPipelineBarriers();   // transition our resources if needed
        command_list_->ExecuteIndirect(dispatch_rays_signature_, 1, gfx_buffer.resource_, gfx_buffer.data_offset_, nullptr, 0);
        return kGfxResult_NoError;
    }

    GfxResult encodeDrawMesh(uint32_t num_groups_x, uint32_t num_groups_y, uint32_t num_groups_z)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(mesh_command_list_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!num_groups_x || !num_groups_y || !num_groups_z)
            return kGfxResult_NoError;  // nothing to draw
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isMesh())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw using a non-mesh kernel object");
        if(kernel.root_signature_ == nullptr || kernel.pipeline_state_ == nullptr)
            return kGfxResult_NoError;  // skip draw call
        GFX_TRY(installShaderState(kernel));
        submitPipelineBarriers();   // transition our resources if needed
        mesh_command_list_->DispatchMesh(num_groups_x, num_groups_y, num_groups_z);
        return kGfxResult_NoError;
    }

    GfxResult encodeDrawMeshIndirect(GfxBuffer args_buffer)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(mesh_command_list_ == nullptr)
            return kGfxResult_InvalidOperation; // avoid spamming console output
        if(!buffer_handles_.has_handle(args_buffer.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot draw using an invalid arguments buffer object");
        if(args_buffer.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw using an arguments buffer object with read CPU access");
        if(!kernel_handles_.has_handle(bound_kernel_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw when bound kernel object is invalid");
        Kernel &kernel = kernels_[bound_kernel_];
        if(!kernel.isMesh())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw using a non-mesh kernel object");
        if(kernel.root_signature_ == nullptr || kernel.pipeline_state_ == nullptr)
            return kGfxResult_NoError;  // skip draw call
        Buffer &gfx_buffer = buffers_[args_buffer];
        SetObjectName(gfx_buffer, args_buffer.name);
        GFX_TRY(installShaderState(kernel));
        if(args_buffer.cpu_access == kGfxCpuAccess_None)
            transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, kTransitionType_Implicit);
        submitPipelineBarriers();   // transition our resources if needed
        command_list_->ExecuteIndirect(draw_mesh_signature_, 1, gfx_buffer.resource_, gfx_buffer.data_offset_, nullptr, 0);
        return kGfxResult_NoError;
    }

    GfxTimestampQuery createTimestampQuery()
    {
        GfxTimestampQuery timestamp_query = {};
        if(isInterop())
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create timestamp query objects when using an interop context");
            return timestamp_query; // invalid operation
        }
        timestamp_query.handle = timestamp_query_handles_.allocate_handle();
        timestamp_queries_.insert(timestamp_query);
        return timestamp_query;
    }

    GfxResult destroyTimestampQuery(GfxTimestampQuery const &timestamp_query)
    {
        if(!timestamp_query)
            return kGfxResult_NoError;
        if(!timestamp_query_handles_.has_handle(timestamp_query.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot destroy invalid timestamp query object");
        timestamp_queries_.erase(timestamp_query);  // destroy timestamp query
        timestamp_query_handles_.free_handle(timestamp_query.handle);
        return kGfxResult_NoError;
    }

    float getTimestampQueryDuration(GfxTimestampQuery const &timestamp_query)
    {
        if(!timestamp_query_handles_.has_handle(timestamp_query.handle))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot get the duration of an invalid timestamp query object");
            return 0.0f;
        }
        return timestamp_queries_[timestamp_query].duration_;
    }

    GfxResult encodeBeginTimestampQuery(GfxTimestampQuery const &timestamp_query)
    {
        if(!timestamp_query_handles_.has_handle(timestamp_query.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot begin a timed section using an invalid timestamp query object");
        TimestampQuery &gfx_timestamp_query = timestamp_queries_[timestamp_query];
        if(gfx_timestamp_query.was_begun_)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot begin a timed section on a timestamp query object that was already open");
        TimestampQueryHeap &timestamp_query_heap = timestamp_query_heaps_[fence_index_];
        if(timestamp_query_heap.timestamp_queries_.find(timestamp_query.handle) != timestamp_query_heap.timestamp_queries_.end())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot use a timestamp query object more than once per frame");
        uint32_t timestamp_query_index = (uint32_t)timestamp_query_heap.timestamp_queries_.size();
        uint64_t timestamp_query_heap_size = 2 * (timestamp_query_index + 1) * sizeof(uint64_t);
        if(timestamp_query_heap_size > timestamp_query_heap.query_buffer_.size)
        {
            ID3D12QueryHeap *query_heap = nullptr;
            timestamp_query_heap_size += (timestamp_query_heap_size + 2) >> 1;
            timestamp_query_heap_size = GFX_ALIGN(timestamp_query_heap_size, 65536);
            D3D12_QUERY_HEAP_DESC
            query_heap_desc       = {};
            query_heap_desc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            query_heap_desc.Count = (uint32_t)(timestamp_query_heap_size >> 3);
            device_->CreateQueryHeap(&query_heap_desc, IID_PPV_ARGS(&query_heap));
            if(query_heap == nullptr)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to create query heap for timestamp query object");
            GfxBuffer query_buffer = createBuffer(timestamp_query_heap_size, nullptr, kGfxCpuAccess_Read);
            if(!query_buffer)
            {
                query_heap->Release();  // release resource
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to allocate memory for timestamp query heap buffer");
            }
            query_buffer.setName("gfx_TimestampQueryHeapBuffer");
            collect(timestamp_query_heap.query_heap_);
            destroyBuffer(timestamp_query_heap.query_buffer_);
            timestamp_query_heap.timestamp_queries_.clear();
            timestamp_query_heap.query_buffer_ = query_buffer;
            timestamp_query_heap.query_heap_ = query_heap;
        }
        GFX_ASSERT(command_list_ != nullptr);
        GFX_ASSERT(timestamp_query_heap.query_heap_ != nullptr);
        timestamp_query_index = (uint32_t)timestamp_query_heap.timestamp_queries_.size();
        command_list_->EndQuery(timestamp_query_heap.query_heap_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * timestamp_query_index + 0);
        timestamp_query_heap.timestamp_queries_[timestamp_query.handle] = std::pair<uint32_t, GfxTimestampQuery>(timestamp_query_index, timestamp_query);
        gfx_timestamp_query.was_begun_ = true;
        return kGfxResult_NoError;
    }

    GfxResult encodeEndTimestampQuery(GfxTimestampQuery const &timestamp_query)
    {
        if(!timestamp_query_handles_.has_handle(timestamp_query.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot end a timed section using an invalid timestamp query object");
        TimestampQuery &gfx_timestamp_query = timestamp_queries_[timestamp_query];
        if(!gfx_timestamp_query.was_begun_)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot end a timed section using a timestamp query object that was already closed");
        TimestampQueryHeap &timestamp_query_heap = timestamp_query_heaps_[fence_index_];
        gfx_timestamp_query.was_begun_ = false; // timestamp query is now closed
        std::map<uint64_t, std::pair<uint32_t, GfxTimestampQuery>>::const_iterator const it = timestamp_query_heap.timestamp_queries_.find(timestamp_query.handle);
        GFX_ASSERT(it != timestamp_query_heap.timestamp_queries_.end());
        if(it == timestamp_query_heap.timestamp_queries_.end())
            return kGfxResult_InternalError;    // should never happen
        uint32_t const timestamp_query_index = (*it).second.first;
        command_list_->EndQuery(timestamp_query_heap.query_heap_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * timestamp_query_index + 1);
        return kGfxResult_NoError;
    }

    GfxResult encodeBeginEvent(uint64_t color, char const *format, va_list args)
    {
        char buffer[4096];
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        vsnprintf(buffer, sizeof(buffer), format, args);
        IAmdExtD3DDevice1 *amd_ext_device = tls_pAmdExtDeviceObject;
        PIXBeginEvent(command_list_, color, buffer);
        if(amd_ext_device == nullptr && tls_pAmdExtDeviceObject != nullptr)
            amd_ext_devices_.push_back(tls_pAmdExtDeviceObject);
        return kGfxResult_NoError;
    }

    GfxResult encodeEndEvent()
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        IAmdExtD3DDevice1 *amd_ext_device = tls_pAmdExtDeviceObject;
        PIXEndEvent(command_list_);
        if(amd_ext_device == nullptr && tls_pAmdExtDeviceObject != nullptr)
            amd_ext_devices_.push_back(tls_pAmdExtDeviceObject);
        return kGfxResult_NoError;
    }

    enum OpType
    {
        kOpType_Min = 0,
        kOpType_Max,
        kOpType_Sum,

        kOpType_Count
    };

    GfxResult encodeScan(OpType op_type, GfxDataType data_type, GfxBuffer const &dst, GfxBuffer const &src, GfxBuffer const *count)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(data_type < 0 || data_type >= kGfxDataType_Count)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot scan buffer object of unsupported data type `%u'", data_type);
        if(dst.size != src.size)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot scan if source and destination buffer objects aren't of the same size");
        if((dst.size >> 2) > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot scan buffer object of more than 4 billion keys");
        if(dst.cpu_access == kGfxCpuAccess_Read || src.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot scan buffer object with read CPU access");
        if(!buffer_handles_.has_handle(dst.handle) || !buffer_handles_.has_handle(src.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot scan when supplied buffer object is invalid");
        if(count != nullptr && !buffer_handles_.has_handle(count->handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot scan when supplied count buffer object is invalid");
        ScanKernels const &scan_kernels = getScanKernels(op_type, data_type, count);
        uint32_t const group_size = 256;
        uint32_t const keys_per_thread = 4;
        uint32_t const keys_per_group = group_size * keys_per_thread;
        uint32_t const num_keys = (uint32_t)(src.size >> 2);
        uint32_t const num_groups_level_1 = (num_keys + keys_per_group - 1) / keys_per_group;
        uint32_t const num_groups_level_2 = (num_groups_level_1 + keys_per_group - 1) / keys_per_group;
        if(num_groups_level_2 > keys_per_group)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot scan buffer object of more than 1 billion keys"); // TODO: implement 3-level scan? (gboisse)
        uint64_t scratch_buffer_size = ((uint64_t)num_groups_level_1 + num_groups_level_2 + 10) << 2;
        if(texture_upload_buffer_.size < scratch_buffer_size)
        {
            destroyBuffer(texture_upload_buffer_);
            scratch_buffer_size += (scratch_buffer_size + 2) >> 1;
            scratch_buffer_size = GFX_ALIGN(scratch_buffer_size, 65536);
            texture_upload_buffer_ = createBuffer(scratch_buffer_size, nullptr, kGfxCpuAccess_None);
            if(!texture_upload_buffer_)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to allocate scratch memory for scan");
            texture_upload_buffer_.setName("gfx_TextureUploadBuffer");
        }
        GfxBuffer const scan_level_1_buffer = createBufferRange(texture_upload_buffer_, 0, (uint64_t)num_groups_level_1 << 2);
        GfxBuffer const scan_level_2_buffer = createBufferRange(texture_upload_buffer_, (uint64_t)num_groups_level_1 << 2, (uint64_t)num_groups_level_2 << 2);
        GfxBuffer const num_groups_level_1_buffer = createBufferRange(texture_upload_buffer_, ((uint64_t)num_groups_level_1 + num_groups_level_2) << 2, 4);
        GfxBuffer const num_groups_level_2_buffer = createBufferRange(texture_upload_buffer_, ((uint64_t)num_groups_level_1 + num_groups_level_2 + 1) << 2, 4);
        GfxBuffer const scan_level_1_args_buffer = createBufferRange(texture_upload_buffer_, ((uint64_t)num_groups_level_1 + num_groups_level_2 + 2) << 2, 4 << 2);
        GfxBuffer const scan_level_2_args_buffer = createBufferRange(texture_upload_buffer_, ((uint64_t)num_groups_level_1 + num_groups_level_2 + 6) << 2, 4 << 2);
        if(!scan_level_1_buffer || !scan_level_2_buffer || !num_groups_level_1_buffer || !num_groups_level_2_buffer || !scan_level_1_args_buffer || !scan_level_2_args_buffer)
        {
            destroyBuffer(scan_level_1_buffer); destroyBuffer(scan_level_2_buffer);
            destroyBuffer(scan_level_1_args_buffer); destroyBuffer(scan_level_2_args_buffer);
            destroyBuffer(num_groups_level_1_buffer); destroyBuffer(num_groups_level_2_buffer);
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create scratch memory for scan");
        }
        GfxKernel const bound_kernel = bound_kernel_;
        if(count != nullptr)
        {
            setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", *count);
            setProgramBuffer(scan_kernels.scan_program_, "g_Args1Buffer", scan_level_1_args_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_Args2Buffer", scan_level_2_args_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_Count1Buffer", num_groups_level_1_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_Count2Buffer", num_groups_level_2_buffer);
            encodeBindKernel(scan_kernels.args_kernel_);
            encodeDispatch(1, 1, 1);
        }
        if(num_keys > keys_per_group)
        {
            if(count != nullptr)
                setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", *count);
            else
                setProgramConstants(scan_kernels.scan_program_, "g_Count", &num_keys, sizeof(num_keys));
            setProgramBuffer(scan_kernels.scan_program_, "g_PartialResults", scan_level_1_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_InputKeys", src);
            encodeBindKernel(scan_kernels.reduce_kernel_);
            if(count != nullptr)
                encodeDispatchIndirect(scan_level_1_args_buffer);
            else
                encodeDispatch(num_groups_level_1, 1, 1);
            if(num_groups_level_1 > keys_per_group)
            {
                if(count != nullptr)
                    setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", num_groups_level_1_buffer);
                else
                    setProgramConstants(scan_kernels.scan_program_, "g_Count", &num_groups_level_1, sizeof(num_groups_level_1));
                setProgramBuffer(scan_kernels.scan_program_, "g_PartialResults", scan_level_2_buffer);
                setProgramBuffer(scan_kernels.scan_program_, "g_InputKeys", scan_level_1_buffer);
                if(count != nullptr)
                    encodeDispatchIndirect(scan_level_2_args_buffer);
                else
                    encodeDispatch(num_groups_level_2, 1, 1);
                {
                    if(count != nullptr)
                        setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", num_groups_level_2_buffer);
                    else
                        setProgramConstants(scan_kernels.scan_program_, "g_Count", &num_groups_level_2, sizeof(num_groups_level_2));
                    setProgramBuffer(scan_kernels.scan_program_, "g_InputKeys", scan_level_2_buffer);
                    setProgramBuffer(scan_kernels.scan_program_, "g_OutputKeys", scan_level_2_buffer);
                    encodeBindKernel(scan_kernels.scan_kernel_);
                    encodeDispatch(1, 1, 1);
                }
            }
            if(count != nullptr)
                setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", num_groups_level_1_buffer);
            else
                setProgramConstants(scan_kernels.scan_program_, "g_Count", &num_groups_level_1, sizeof(num_groups_level_1));
            setProgramBuffer(scan_kernels.scan_program_, "g_InputKeys", scan_level_1_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_OutputKeys", scan_level_1_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_PartialResults", scan_level_2_buffer);
            encodeBindKernel(num_groups_level_1 > keys_per_group ? scan_kernels.scan_add_kernel_ : scan_kernels.scan_kernel_);
            if(count != nullptr)
                encodeDispatchIndirect(scan_level_2_args_buffer);
            else
                encodeDispatch(num_groups_level_2, 1, 1);
        }
        if(count != nullptr)
            setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", *count);
        else
            setProgramConstants(scan_kernels.scan_program_, "g_Count", &num_keys, sizeof(num_keys));
        setProgramBuffer(scan_kernels.scan_program_, "g_InputKeys", src);
        setProgramBuffer(scan_kernels.scan_program_, "g_OutputKeys", dst);
        setProgramBuffer(scan_kernels.scan_program_, "g_PartialResults", scan_level_1_buffer);
        encodeBindKernel(num_keys > keys_per_group ? scan_kernels.scan_add_kernel_ : scan_kernels.scan_kernel_);
        if(count != nullptr)
            encodeDispatchIndirect(scan_level_1_args_buffer);
        else
            encodeDispatch(num_groups_level_1, 1, 1);
        destroyBuffer(scan_level_1_buffer); destroyBuffer(scan_level_2_buffer);
        destroyBuffer(scan_level_1_args_buffer); destroyBuffer(scan_level_2_args_buffer);
        destroyBuffer(num_groups_level_1_buffer); destroyBuffer(num_groups_level_2_buffer);
        if(kernel_handles_.has_handle(bound_kernel.handle))
            encodeBindKernel(bound_kernel);
        else
            bound_kernel_ = bound_kernel;
        return kGfxResult_NoError;
    }

    GfxResult encodeReduce(OpType op_type, GfxDataType data_type, GfxBuffer const &dst, GfxBuffer const &src, GfxBuffer const *count)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(data_type < 0 || data_type >= kGfxDataType_Count)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot reduce buffer object of unsupported data type `%u'", data_type);
        if(dst.size < 4)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot reduce if destination buffer object isn't at least 4 bytes large");
        if((src.size >> 2) > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot reduce buffer object of more than 4 billion keys");
        if(dst.cpu_access == kGfxCpuAccess_Read || src.cpu_access == kGfxCpuAccess_Read)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot reduce buffer object with read CPU access");
        if(!buffer_handles_.has_handle(dst.handle) || !buffer_handles_.has_handle(src.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot reduce when supplied buffer object is invalid");
        if(count != nullptr && !buffer_handles_.has_handle(count->handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot reduce when supplied count buffer object is invalid");
        ScanKernels const &scan_kernels = getScanKernels(op_type, data_type, count);
        uint32_t const group_size = 256;
        uint32_t const keys_per_thread = 4;
        uint32_t const keys_per_group = group_size * keys_per_thread;
        uint32_t const num_keys = (uint32_t)(src.size >> 2);
        uint32_t const num_groups_level_1 = (num_keys + keys_per_group - 1) / keys_per_group;
        uint32_t const num_groups_level_2 = (num_groups_level_1 + keys_per_group - 1) / keys_per_group;
        if(num_groups_level_2 > keys_per_group)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot reduce buffer object of more than 1 billion keys");   // TODO: implement 3-level reduction? (gboisse)
        uint64_t scratch_buffer_size = ((uint64_t)num_groups_level_1 + num_groups_level_2 + 10) << 2;
        if(texture_upload_buffer_.size < scratch_buffer_size)
        {
            destroyBuffer(texture_upload_buffer_);
            scratch_buffer_size += (scratch_buffer_size + 2) >> 1;
            scratch_buffer_size = GFX_ALIGN(scratch_buffer_size, 65536);
            texture_upload_buffer_ = createBuffer(scratch_buffer_size, nullptr, kGfxCpuAccess_None);
            if(!texture_upload_buffer_)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to allocate scratch memory for scan");
            texture_upload_buffer_.setName("gfx_TextureUploadBuffer");
        }
        GfxBuffer const reduce_level_1_buffer = createBufferRange(texture_upload_buffer_, 0, (uint64_t)num_groups_level_1 << 2);
        GfxBuffer const reduce_level_2_buffer = createBufferRange(texture_upload_buffer_, (uint64_t)num_groups_level_1 << 2, (uint64_t)num_groups_level_2 << 2);
        GfxBuffer const num_groups_level_1_buffer = createBufferRange(texture_upload_buffer_, ((uint64_t)num_groups_level_1 + num_groups_level_2) << 2, 4);
        GfxBuffer const num_groups_level_2_buffer = createBufferRange(texture_upload_buffer_, ((uint64_t)num_groups_level_1 + num_groups_level_2 + 1) << 2, 4);
        GfxBuffer const reduce_level_1_args_buffer = createBufferRange(texture_upload_buffer_, ((uint64_t)num_groups_level_1 + num_groups_level_2 + 2) << 2, 4 << 2);
        GfxBuffer const reduce_level_2_args_buffer = createBufferRange(texture_upload_buffer_, ((uint64_t)num_groups_level_1 + num_groups_level_2 + 6) << 2, 4 << 2);
        if(!reduce_level_1_buffer || !reduce_level_2_buffer || !num_groups_level_1_buffer || !num_groups_level_2_buffer || !reduce_level_1_args_buffer || !reduce_level_2_args_buffer)
        {
            destroyBuffer(reduce_level_1_buffer); destroyBuffer(reduce_level_2_buffer);
            destroyBuffer(num_groups_level_1_buffer); destroyBuffer(num_groups_level_2_buffer);
            destroyBuffer(reduce_level_1_args_buffer); destroyBuffer(reduce_level_2_args_buffer);
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create scratch memory for scan");
        }
        GfxKernel const bound_kernel = bound_kernel_;
        if(count != nullptr)
        {
            setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", *count);
            setProgramBuffer(scan_kernels.scan_program_, "g_Args1Buffer", reduce_level_1_args_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_Args2Buffer", reduce_level_2_args_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_Count1Buffer", num_groups_level_1_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_Count2Buffer", num_groups_level_2_buffer);
            encodeBindKernel(scan_kernels.args_kernel_);
            encodeDispatch(1, 1, 1);
        }
        encodeBindKernel(scan_kernels.reduce_kernel_);
        if(num_keys > keys_per_group)
        {
            if(count != nullptr)
                setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", *count);
            else
                setProgramConstants(scan_kernels.scan_program_, "g_Count", &num_keys, sizeof(num_keys));
            setProgramBuffer(scan_kernels.scan_program_, "g_PartialResults", reduce_level_1_buffer);
            setProgramBuffer(scan_kernels.scan_program_, "g_InputKeys", src);
            if(count != nullptr)
                encodeDispatchIndirect(reduce_level_1_args_buffer);
            else
                encodeDispatch(num_groups_level_1, 1, 1);
            if(num_groups_level_1 > keys_per_group)
            {
                if(count != nullptr)
                    setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", num_groups_level_1_buffer);
                else
                    setProgramConstants(scan_kernels.scan_program_, "g_Count", &num_groups_level_1, sizeof(num_groups_level_1));
                setProgramBuffer(scan_kernels.scan_program_, "g_PartialResults", reduce_level_2_buffer);
                setProgramBuffer(scan_kernels.scan_program_, "g_InputKeys", reduce_level_1_buffer);
                if(count != nullptr)
                    encodeDispatchIndirect(reduce_level_2_args_buffer);
                else
                    encodeDispatch(num_groups_level_2, 1, 1);
            }
        }
        if(count != nullptr)
            setProgramBuffer(scan_kernels.scan_program_, "g_CountBuffer", num_keys > keys_per_group ? num_groups_level_1 > keys_per_group ? num_groups_level_2_buffer : num_groups_level_1_buffer : *count);
        else
            setProgramConstants(scan_kernels.scan_program_, "g_Count", num_keys > keys_per_group ? num_groups_level_1 > keys_per_group ? &num_groups_level_2 : &num_groups_level_1 : &num_keys, sizeof(num_keys));
        setProgramBuffer(scan_kernels.scan_program_, "g_PartialResults", dst);
        setProgramBuffer(scan_kernels.scan_program_, "g_InputKeys", num_keys > keys_per_group ? num_groups_level_1 > keys_per_group ? reduce_level_2_buffer : reduce_level_1_buffer : src);
        encodeDispatch(1, 1, 1);    // reduce to output
        destroyBuffer(reduce_level_1_buffer); destroyBuffer(reduce_level_2_buffer);
        destroyBuffer(num_groups_level_1_buffer); destroyBuffer(num_groups_level_2_buffer);
        destroyBuffer(reduce_level_1_args_buffer); destroyBuffer(reduce_level_2_args_buffer);
        if(kernel_handles_.has_handle(bound_kernel.handle))
            encodeBindKernel(bound_kernel);
        else
            bound_kernel_ = bound_kernel;
        return kGfxResult_NoError;
    }

    GfxResult encodeRadixSort(GfxBuffer const &keys_dst, GfxBuffer const &keys_src, GfxBuffer const *values_dst, GfxBuffer const *values_src, GfxBuffer const *count)
    {
        if(command_list_ == nullptr)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot encode without a valid command list");
        if(keys_dst.size != keys_src.size)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot sort if source and destination buffer objects aren't of the same size");
        if(values_dst != nullptr && keys_dst.size != values_dst->size)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot sort if keys and values buffer objects aren't of the same size");
        if(values_dst != nullptr && values_src != nullptr && values_dst->size != values_src->size)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot sort if source and destination buffer objects aren't of the same size");
        if((keys_dst.size >> 2) > 0xFFFFFFFFull)
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot sort buffer object of more than 4 billion keys");
        if(!buffer_handles_.has_handle(keys_dst.handle) || !buffer_handles_.has_handle(keys_src.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot sort keys when supplied buffer object is invalid");
        if((values_dst != nullptr && !buffer_handles_.has_handle(values_dst->handle)) || (values_src != nullptr && !buffer_handles_.has_handle(values_src->handle)))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot sort values when supplied buffer object is invalid");
        if((values_dst != nullptr && values_src == nullptr) || (values_dst == nullptr && values_src != nullptr))
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot sort values if source or destination isn't a valid buffer object");
        if(count != nullptr && !buffer_handles_.has_handle(count->handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot sort when supplied count buffer object is invalid");
        SortKernels const &sort_kernels = getSortKernels(values_src != nullptr, count);
        uint32_t const group_size = 256;
        uint32_t const keys_per_thread = 4;
        uint32_t const num_bits_per_pass = 4;
        uint32_t const num_bins = (1 << num_bits_per_pass);
        uint32_t const keys_per_group = group_size * keys_per_thread;
        uint32_t const num_keys = (uint32_t)(keys_src.size >> 2);
        uint32_t const num_groups = (num_keys + keys_per_group - 1) / keys_per_group;
        uint32_t const num_histogram_values = num_bins * num_groups;
        uint64_t scratch_buffer_size = (2ULL * num_keys + num_histogram_values + 5) << 2;
        if(sort_scratch_buffer_.size < scratch_buffer_size)
        {
            destroyBuffer(sort_scratch_buffer_);
            scratch_buffer_size += (scratch_buffer_size + 2) >> 1;
            scratch_buffer_size = GFX_ALIGN(scratch_buffer_size, 65536);
            sort_scratch_buffer_ = createBuffer(scratch_buffer_size, nullptr, kGfxCpuAccess_None);
            if(!sort_scratch_buffer_)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to allocate scratch memory for sort");
            sort_scratch_buffer_.setName("gfx_SortScratchBuffer");
        }
        GfxBuffer const scratch_keys = createBufferRange(sort_scratch_buffer_, 0, (uint64_t)num_keys << 2);
        GfxBuffer const scratch_values = createBufferRange(sort_scratch_buffer_, (uint64_t)num_keys << 2, (uint64_t)num_keys << 2);
        GfxBuffer const group_histograms = createBufferRange(sort_scratch_buffer_, (2ULL * num_keys) << 2, (uint64_t)num_histogram_values << 2);
        GfxBuffer const args_buffer = createBufferRange(sort_scratch_buffer_, (2ULL * num_keys + num_histogram_values) << 2, 4 << 2);
        GfxBuffer const count_buffer = createBufferRange(sort_scratch_buffer_, (2ULL * num_keys + num_histogram_values + 4) << 2, 4);
        if(!scratch_keys || !scratch_values || !group_histograms || !args_buffer || !count_buffer)
        {
            destroyBuffer(group_histograms);
            destroyBuffer(args_buffer); destroyBuffer(count_buffer);
            destroyBuffer(scratch_keys); destroyBuffer(scratch_values);
            return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to create scratch memory for sort");
        }
        GfxKernel const bound_kernel = bound_kernel_;
        setProgramConstants(sort_kernels.sort_program_, "g_Count", &num_keys, sizeof(num_keys));
        if(count != nullptr)
        {
            setProgramBuffer(sort_kernels.sort_program_, "g_CountBuffer", *count);
            setProgramBuffer(sort_kernels.sort_program_, "g_ArgsBuffer", args_buffer);
            setProgramBuffer(sort_kernels.sort_program_, "g_ScanCountBuffer", count_buffer);
            encodeBindKernel(sort_kernels.args_kernel_);
            encodeDispatch(1, 1, 1);
        }
        setProgramBuffer(sort_kernels.sort_program_, "g_GroupHistograms", group_histograms);
        for(uint32_t bitshift = 0, i = 0; bitshift < 32; bitshift += num_bits_per_pass, ++i)
        {
            setProgramConstants(sort_kernels.sort_program_, "g_Bitshift", &bitshift, sizeof(bitshift));
            setProgramBuffer(sort_kernels.sort_program_, "g_InputKeys", i == 0 ? keys_src : (i & 1) != 0 ? scratch_keys : keys_dst);
            setProgramBuffer(sort_kernels.sort_program_, "g_OutputKeys", (i & 1) == 0 ? scratch_keys : keys_dst);
            if(values_dst != nullptr)
            {
                setProgramBuffer(sort_kernels.sort_program_, "g_InputValues", i == 0 ? *values_src : (i & 1) != 0 ? scratch_values : *values_dst);
                setProgramBuffer(sort_kernels.sort_program_, "g_OutputValues", (i & 1) == 0 ? scratch_values : *values_dst);
            }
            encodeBindKernel(sort_kernels.histogram_kernel_);
            if(count != nullptr)
                encodeDispatchIndirect(args_buffer);
            else
                encodeDispatch(num_groups, 1, 1);
            encodeScan(kOpType_Sum, kGfxDataType_Uint, group_histograms, group_histograms, &count_buffer);
            encodeBindKernel(sort_kernels.scatter_kernel_);
            if(count != nullptr)
                encodeDispatchIndirect(args_buffer);
            else
                encodeDispatch(num_groups, 1, 1);
        }
        destroyBuffer(group_histograms);
        destroyBuffer(args_buffer); destroyBuffer(count_buffer);
        destroyBuffer(scratch_keys); destroyBuffer(scratch_values);
        if(kernel_handles_.has_handle(bound_kernel.handle))
            encodeBindKernel(bound_kernel);
        else
            bound_kernel_ = bound_kernel;
        return kGfxResult_NoError;
    }

    void decayResourceState()
    {
        for(uint32_t i = 0; i < buffers_.size(); ++i)
        {
            auto *buffer = buffers_.at(i);
            if(buffer != nullptr && buffer->initial_resource_state_ == D3D12_RESOURCE_STATE_COMMON && !*buffer->transitioned_ && (*buffer->resource_state_ & D3D12_RESOURCE_STATE_GENERIC_READ) == *buffer->resource_state_)
                *buffer->resource_state_ = D3D12_RESOURCE_STATE_COMMON;
        }
        for(uint32_t i = 0; i < textures_.size(); ++i)
        {
            auto *texture = textures_.at(i);
            if(texture != nullptr && !texture->transitioned_ && (texture->resource_state_ & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) == texture->resource_state_)
                texture->resource_state_ = D3D12_RESOURCE_STATE_COMMON;
        }
        if(dbg_command_list_ != nullptr)
        {
            for(uint32_t i = 0; i < buffers_.size(); ++i)
            {
                auto *buffer = buffers_.at(i);
                if(buffer != nullptr)
                {
                    dbg_command_list_->AssertResourceState(buffer->resource_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, *buffer->resource_state_);
                }
            }
            for(uint32_t i = 0; i < textures_.size(); ++i)
            {
                auto *texture = textures_.at(i);
                if(texture != nullptr)
                {
                    dbg_command_list_->AssertResourceState(texture->resource_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, texture->resource_state_);
                }
            }
        }
    }

    GfxResult frame(bool vsync)
    {
        GFX_ASSERT(max_frames_in_flight_ > 0);
        if(isInterop())
            fence_index_ = (fence_index_ + 1) % max_frames_in_flight_;
        else
        {
            if(!timestamp_query_heaps_[fence_index_].timestamp_queries_.empty())
            {
                for(std::map<uint64_t, std::pair<uint32_t, GfxTimestampQuery>>::const_iterator it = timestamp_query_heaps_[fence_index_].timestamp_queries_.begin(); it != timestamp_query_heaps_[fence_index_].timestamp_queries_.end(); ++it)
                {
                    if(!timestamp_query_handles_.has_handle((*it).second.second.handle))
                        continue;   // timestamp query object was destroyed
                    TimestampQuery const &timestamp_query = timestamp_queries_[(*it).second.second];
                    if(timestamp_query.was_begun_)  // was the query not closed properly?
                        encodeEndTimestampQuery((*it).second.second);
                    GFX_ASSERT(!timestamp_query.was_begun_);
                }
                uint32_t const timestamp_query_count = (uint32_t)timestamp_query_heaps_[fence_index_].timestamp_queries_.size();
                GFX_ASSERT(buffer_handles_.has_handle(timestamp_query_heaps_[fence_index_].query_buffer_.handle));
                Buffer const &query_buffer = buffers_[timestamp_query_heaps_[fence_index_].query_buffer_];
                command_list_->ResolveQueryData(timestamp_query_heaps_[fence_index_].query_heap_,
                    D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * timestamp_query_count, query_buffer.resource_, query_buffer.data_offset_);
            }
            if(swap_chain_ != nullptr)
            {
                RECT window_rect = {};
                GetClientRect(window_, &window_rect);
                D3D12_RESOURCE_BARRIER resource_barrier = {};
                resource_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                resource_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                resource_barrier.Transition.pResource = back_buffers_[fence_index_];
                resource_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                resource_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                command_list_->ResourceBarrier(1, &resource_barrier);
                command_list_->Close(); // close command list for submit
                ID3D12CommandList *const command_lists[] = { command_list_ };
                command_queue_->ExecuteCommandLists(ARRAYSIZE(command_lists), command_lists);
                command_queue_->Signal(fences_[fence_index_], ++fence_values_[fence_index_]);
                HRESULT hr = swap_chain_->Present(vsync ? 1 : 0, vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
                if(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
                {
                    GFX_TRY(handleDeviceLost());
                }
                else if(FAILED(hr))
                {
                    return GFX_SET_ERROR(kGfxResult_InternalError, "Error detected during present: %s", hr);
                }
                uint32_t const window_width  = GFX_MAX(window_rect.right,  (LONG)8);
                uint32_t const window_height = GFX_MAX(window_rect.bottom, (LONG)8);
                fence_index_ = swap_chain_->GetCurrentBackBufferIndex();
                if(window_width != window_width_ || window_height != window_height_)
                {
                    GFX_TRY(resizeCallback(window_width, window_height)); // reset fence index
                }
                if(fences_[fence_index_]->GetCompletedValue() != fence_values_[fence_index_])
                {
                    fences_[fence_index_]->SetEventOnCompletion(fence_values_[fence_index_], fence_event_);
                    WaitForSingleObject(fence_event_, INFINITE);    // wait for GPU to complete
                }
                command_allocators_[fence_index_]->Reset();
                command_list_->Reset(command_allocators_[fence_index_], nullptr);
                resource_barrier.Transition.pResource = back_buffers_[fence_index_];
                resource_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                resource_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                command_list_->ResourceBarrier(1, &resource_barrier);
            }
            else
            {
                command_list_->Close(); // close command list for submit
                ID3D12CommandList *const command_lists[] = { command_list_ };
                command_queue_->ExecuteCommandLists(ARRAYSIZE(command_lists), command_lists);
                command_queue_->Signal(fences_[fence_index_], ++fence_values_[fence_index_]);
                fence_index_ = (fence_index_ + 1) % max_frames_in_flight_;
                if(fences_[fence_index_]->GetCompletedValue() != fence_values_[fence_index_])
                {
                    fences_[fence_index_]->SetEventOnCompletion(fence_values_[fence_index_], fence_event_);
                    WaitForSingleObject(fence_event_, INFINITE);    // wait for GPU to complete
                }
                command_allocators_[fence_index_]->Reset();
                command_list_->Reset(command_allocators_[fence_index_], nullptr);
            }
            if(!timestamp_query_heaps_[fence_index_].timestamp_queries_.empty())
            {
                double const ticks_per_milliseconds = timestamp_query_ticks_per_second_ / 1000.0;
                for(std::map<uint64_t, std::pair<uint32_t, GfxTimestampQuery>>::const_iterator it = timestamp_query_heaps_[fence_index_].timestamp_queries_.begin(); it != timestamp_query_heaps_[fence_index_].timestamp_queries_.end(); ++it)
                {
                    if(!timestamp_query_handles_.has_handle((*it).second.second.handle))
                        continue;   // timestamp query object was destroyed
                    uint32_t const timestamp_query_index = (*it).second.first;
                    TimestampQuery &timestamp_query = timestamp_queries_[(*it).second.second];
                    GFX_ASSERT(buffer_handles_.has_handle(timestamp_query_heaps_[fence_index_].query_buffer_.handle));
                    GFX_ASSERT(timestamp_query_index < timestamp_query_heaps_[fence_index_].timestamp_queries_.size());
                    Buffer const &query_buffer = buffers_[timestamp_query_heaps_[fence_index_].query_buffer_];
                    uint64_t const *timestamp_query_data = (uint64_t const *)((char const *)query_buffer.data_ + query_buffer.data_offset_);
                    double const begin = timestamp_query_data[2 * timestamp_query_index + 0] / ticks_per_milliseconds;
                    double const end   = timestamp_query_data[2 * timestamp_query_index + 1] / ticks_per_milliseconds;
                    float const duration    = (float)(GFX_MAX(begin, end) - begin); // elapsed time in milliseconds
                    float const lerp_amount = (fabsf(duration - timestamp_query.duration_) / GFX_MAX(duration, 1e-3f) > 1.0f ? 0.0f : 0.95f);
                    timestamp_query.duration_ = duration * (1.0f - lerp_amount) + timestamp_query.duration_ * lerp_amount;
                }
                timestamp_query_heaps_[fence_index_].timestamp_queries_.clear();
            }
            decayResourceState();
        }
        constant_buffer_pool_cursors_[fence_index_] = 0;
        resetState();   // re-install state
        return runGarbageCollection();
    }

    GfxResult finish()
    {
        if(isInterop())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot synchronize commands when using an interop context");
        command_list_->Close(); // close command list for submit
        ID3D12CommandList *const command_lists[] = { command_list_ };
        command_queue_->ExecuteCommandLists(ARRAYSIZE(command_lists), command_lists);
        GFX_TRY(sync());    // make sure GPU has gone through all pending work
        command_allocators_[fence_index_]->Reset();
        command_list_->Reset(command_allocators_[fence_index_], nullptr);
        resetState();   // re-install state
        decayResourceState();
        return kGfxResult_NoError;
    }

    inline ID3D12Device *getDevice() const
    {
        return device_;
    }

    inline ID3D12CommandQueue *getCommandQueue() const
    {
        return command_queue_;
    }

    inline ID3D12GraphicsCommandList *getCommandList() const
    {
        return command_list_;
    }

    GfxResult setCommandList(ID3D12GraphicsCommandList *command_list)
    {
        if(!isInterop())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot set command list on a non-interop context");
        if(command_list_ != nullptr)
            command_list_->Release();
        if(dxr_command_list_ != nullptr)
            dxr_command_list_->Release();
        if(mesh_command_list_ != nullptr)
            mesh_command_list_->Release();
        command_list_ = command_list;
        dxr_command_list_ = nullptr;
        mesh_command_list_ = nullptr;
        if(command_list_ != nullptr)
        {
            command_list_->QueryInterface(IID_PPV_ARGS(&dxr_command_list_));
            command_list_->QueryInterface(IID_PPV_ARGS(&mesh_command_list_));
            command_list_->AddRef();    // retain command list
            if(dxr_command_list_ == nullptr)
            {
                if(dxr_device_ != nullptr)
                    dxr_device_->Release();
                dxr_device_ = nullptr;
            }
            else if(dxr_device_ == nullptr)
            {
                dxr_command_list_->Release();
                dxr_command_list_ = nullptr;
            }
            if(mesh_command_list_ == nullptr)
            {
                if(mesh_device_ != nullptr)
                    mesh_device_->Release();
                mesh_device_ = nullptr;
            }
            else if(mesh_device_ == nullptr)
            {
                mesh_command_list_->Release();
                mesh_command_list_ = nullptr;
            }
            resetState();
        }
        return kGfxResult_NoError;
    }

    GfxResult resetCommandListState()
    {
        resetState();

        return kGfxResult_NoError;
    }

    GfxBuffer createBuffer(ID3D12Resource *resource, D3D12_RESOURCE_STATES resource_state)
    {
        GfxBuffer buffer = {};
        if(resource == nullptr)
            return buffer;  // invalid parameter
        D3D12_RESOURCE_DESC const resource_desc = resource->GetDesc();
        if(resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a buffer object from a non-buffer resource");
            return buffer;  // invalid operation
        }
        buffer.handle = buffer_handles_.allocate_handle();
        Buffer &gfx_buffer = buffers_.insert(buffer);
        buffer.size = (uint64_t)resource_desc.Width;
        buffer.cpu_access = kGfxCpuAccess_None;
        buffer.stride = sizeof(uint32_t);
        gfx_buffer.flags_ = Object::kFlag_Named;
        gfx_buffer.reference_count_ = (uint32_t *)gfxMalloc(sizeof(uint32_t));
        *gfx_buffer.reference_count_ = 1;   // retain
        gfx_buffer.resource_state_ = (D3D12_RESOURCE_STATES *)gfxMalloc(sizeof(D3D12_RESOURCE_STATES));
        gfx_buffer.transitioned_ = (bool *)gfxMalloc(sizeof(bool));
        GFX_ASSERT(gfx_buffer.resource_state_ != nullptr && gfx_buffer.transitioned_ != nullptr);
        *gfx_buffer.resource_state_ = resource_state;
        *gfx_buffer.transitioned_ = false;
        gfx_buffer.initial_resource_state_ = resource_state;
        gfx_buffer.resource_ = resource;
        resource->AddRef(); // retain
        return buffer;
    }

    GfxTexture createTexture(ID3D12Resource *resource, D3D12_RESOURCE_STATES resource_state)
    {
        GfxTexture texture = {};
        if(resource == nullptr)
            return texture; // invalid parameter
        D3D12_RESOURCE_DESC const resource_desc = resource->GetDesc();
        switch(resource_desc.Dimension)
        {
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            texture.type = (resource_desc.DepthOrArraySize > 1 ? GfxTexture::kType_2DArray : GfxTexture::kType_2D);
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            texture.type = GfxTexture::kType_3D;
            break;
        default:
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create a texture object from a non-texture resource");
            return texture; // invalid operation
        }
        if(resource_desc.SampleDesc.Count > 1)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Multisample textures are not supported");
            return texture; // invalid operation
        }
        texture.handle = texture_handles_.allocate_handle();
        Texture &gfx_texture = textures_.insert(texture);
        texture.width = (uint32_t)resource_desc.Width;
        texture.height = (uint32_t)resource_desc.Height;
        texture.depth = (uint32_t)resource_desc.DepthOrArraySize;
        texture.format = resource_desc.Format;
        texture.mip_levels = (uint32_t)resource_desc.MipLevels;
        ResolveClearValueForTexture(gfx_texture, nullptr, texture.format);
        memcpy(texture.clear_value, gfx_texture.clear_value_, sizeof(texture.clear_value));
        if((resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
            for(uint32_t i = 0; i < ARRAYSIZE(gfx_texture.dsv_descriptor_slots_); ++i)
            {
                gfx_texture.dsv_descriptor_slots_[i].resize(resource_desc.DepthOrArraySize);
                for(size_t j = 0; j < gfx_texture.dsv_descriptor_slots_[i].size(); ++j)
                    gfx_texture.dsv_descriptor_slots_[i][j] = 0xFFFFFFFFu;
            }
        if((resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0)
            for(uint32_t i = 0; i < ARRAYSIZE(gfx_texture.rtv_descriptor_slots_); ++i)
            {
                gfx_texture.rtv_descriptor_slots_[i].resize(resource_desc.DepthOrArraySize);
                for(size_t j = 0; j < gfx_texture.rtv_descriptor_slots_[i].size(); ++j)
                    gfx_texture.rtv_descriptor_slots_[i][j] = 0xFFFFFFFFu;
            }
        gfx_texture.Object::flags_ = Object::kFlag_Named;
        gfx_texture.initial_resource_state_ = resource_state;
        gfx_texture.resource_state_ = resource_state;
        gfx_texture.resource_ = resource;
        resource->AddRef(); // retain
        return texture;
    }

    GfxAccelerationStructure createAccelerationStructure(ID3D12Resource *resource, uint64_t byte_offset)
    {
        GfxAccelerationStructure acceleration_structure = {};
        if(resource == nullptr)
            return acceleration_structure;  // invalid parameter
        D3D12_RESOURCE_DESC const resource_desc = resource->GetDesc();
        if(resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create an acceleration structure object from a non-buffer resource");
            return acceleration_structure;  // invalid operation
        }
        if(byte_offset >= (uint64_t)resource_desc.Width)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot have a byte offset that is larger than the size of the buffer resource");
            return acceleration_structure;  // invalid operation
        }
        acceleration_structure.handle = acceleration_structure_handles_.allocate_handle();
        AccelerationStructure &gfx_acceleration_structure = acceleration_structures_.insert(acceleration_structure);
        gfx_acceleration_structure.bvh_buffer_.handle = buffer_handles_.allocate_handle();
        Buffer &gfx_buffer = buffers_.insert(gfx_acceleration_structure.bvh_buffer_);
        gfx_acceleration_structure.bvh_buffer_.size = (uint32_t)resource_desc.Width;
        gfx_acceleration_structure.bvh_buffer_.cpu_access = kGfxCpuAccess_None;
        gfx_acceleration_structure.bvh_buffer_.stride = sizeof(uint32_t);
        gfx_buffer.flags_ = Object::kFlag_Named;
        gfx_buffer.data_offset_ = byte_offset;
        gfx_buffer.reference_count_ = (uint32_t *)gfxMalloc(sizeof(uint32_t));
        *gfx_buffer.reference_count_ = 1;   // retain
        gfx_buffer.resource_state_ = (D3D12_RESOURCE_STATES *)gfxMalloc(sizeof(D3D12_RESOURCE_STATES));
        gfx_buffer.transitioned_ = (bool *)gfxMalloc(sizeof(bool));
        GFX_ASSERT(gfx_buffer.resource_state_ != nullptr && gfx_buffer.transitioned_ != nullptr);
        *gfx_buffer.resource_state_ = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        *gfx_buffer.transitioned_ = false;
        gfx_buffer.initial_resource_state_ = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        gfx_buffer.resource_ = resource;
        resource->AddRef(); // retain
        return acceleration_structure;
    }

    ID3D12Resource *getBufferResource(GfxBuffer const &buffer)
    {
        if(!buffer_handles_.has_handle(buffer.handle))
            return nullptr; // invalid buffer object
        Buffer const &gfx_buffer = buffers_[buffer];
        return gfx_buffer.resource_;
    }

    ID3D12Resource *getTextureResource(GfxTexture const &texture)
    {
        if(!texture_handles_.has_handle(texture.handle))
            return nullptr; // invalid texture object
        Texture const &gfx_texture = textures_[texture];
        return gfx_texture.resource_;
    }

    ID3D12Resource *getAccelerationStructureResource(GfxAccelerationStructure const &acceleration_structure)
    {
        if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
            return nullptr; // invalid acceleration structure object
        AccelerationStructure const &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
        if(!buffer_handles_.has_handle(gfx_acceleration_structure.bvh_buffer_.handle))
            return nullptr; // acceleration structure wasn't built yet
        Buffer const &bvh_buffer = buffers_[gfx_acceleration_structure.bvh_buffer_];
        return bvh_buffer.resource_;
    }

    D3D12_RESOURCE_STATES getBufferResourceState(GfxBuffer const &buffer)
    {
        if(!buffer_handles_.has_handle(buffer.handle))
            return D3D12_RESOURCE_STATE_COMMON; // invalid buffer object
        Buffer const &gfx_buffer = buffers_[buffer];
        return *gfx_buffer.resource_state_;
    }

    D3D12_RESOURCE_STATES getTextureResourceState(GfxTexture const &texture)
    {
        if(!texture_handles_.has_handle(texture.handle))
            return D3D12_RESOURCE_STATE_COMMON; // invalid texture object
        Texture const &gfx_texture = textures_[texture];
        return gfx_texture.resource_state_;
    }

    HANDLE createBufferSharedHandle(GfxBuffer const &buffer)
    {
        HANDLE handle = {};
        if(!buffer_handles_.has_handle(buffer.handle))
        {
            if(!!buffer)
                GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Cannot create shared handle from invalid buffer object");
            return handle;  // invalid buffer object
        }
        if(buffer.cpu_access != kGfxCpuAccess_None)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create shared handle from buffer object with CPU access");
            return handle;  // invalid CPU access mode
        }
        Buffer &gfx_buffer = buffers_[buffer];
        if(gfx_buffer.isInterop())
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create shared handle from interop buffer object");
            return handle;  // invalid buffer object type
        }
        if(*gfx_buffer.reference_count_ != 1)
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot create shared handle from ranged buffer object");
            return handle;  // invalid buffer range
        }
        D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE;
        if(!SUCCEEDED(gfx_buffer.resource_->GetHeapProperties(nullptr, &heap_flags)))
        {
            GFX_PRINT_ERROR(kGfxResult_InternalError, "Cannot query heap information from buffer object");
            return handle;  // internal error
        }
        if(!((heap_flags & D3D12_HEAP_FLAG_SHARED) != 0))
        {
            ID3D12Resource *resource = nullptr;
            D3D12MA::Allocation *allocation = nullptr;
            D3D12_RESOURCE_DESC const resource_desc = gfx_buffer.resource_->GetDesc();
            D3D12MA::ALLOCATION_DESC
            allocation_desc                = {};
            allocation_desc.HeapType       = D3D12_HEAP_TYPE_DEFAULT;
            allocation_desc.ExtraHeapFlags = D3D12_HEAP_FLAG_SHARED;
            if(createResource(allocation_desc, resource_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, &allocation, IID_PPV_ARGS(&resource)) != kGfxResult_NoError)
            {
                GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to create shared buffer object");
                return handle;  // out of memory
            }
            bool transitioned = transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, kTransitionType_Implicit);
            ID3D12Resource *previous_resource = gfx_buffer.resource_;
            collect(gfx_buffer);    // release previous buffer
            gfx_buffer.resource_ = resource;
            gfx_buffer.allocation_ = allocation;
            gfx_buffer.flags_ &= ~Object::kFlag_Named;
            gfx_buffer.reference_count_  = (uint32_t *)gfxMalloc(sizeof(uint32_t));
            *gfx_buffer.reference_count_ = 1;   // retain
            gfx_buffer.resource_state_   = (D3D12_RESOURCE_STATES *)gfxMalloc(sizeof(D3D12_RESOURCE_STATES));
            gfx_buffer.transitioned_     = (bool *)gfxMalloc(sizeof(bool));
            GFX_ASSERT(gfx_buffer.resource_state_ != nullptr && gfx_buffer.transitioned_ != nullptr);
            *gfx_buffer.resource_state_ = D3D12_RESOURCE_STATE_COMMON;
            gfx_buffer.initial_resource_state_ = D3D12_RESOURCE_STATE_COMMON;
            *gfx_buffer.transitioned_   = false;
            transitioned |= transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit);
            if(transitioned) submitPipelineBarriers();
            command_list_->CopyResource(gfx_buffer.resource_, previous_resource);
        }
        WCHAR wname[ARRAYSIZE(buffer.name)] = {};
        WindowsSecurityAttributes security_attributes;
        mbstowcs(wname, buffer.name, ARRAYSIZE(buffer.name));
        if(!SUCCEEDED(device_->CreateSharedHandle(gfx_buffer.resource_, &security_attributes, GENERIC_ALL, wname, &handle)))
            GFX_PRINT_ERROR(kGfxResult_InternalError, "Failed to create shared handle from buffer object");
        return handle;
    }

    GfxResult execute()
    {
        if(isInterop())
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot synchronize commands when using an interop context");
        command_list_->Close(); // close command list for submit
        ID3D12CommandList *const command_lists[] = { command_list_ };
        command_queue_->ExecuteCommandLists(ARRAYSIZE(command_lists), command_lists);

        return kGfxResult_NoError;
    }

    GfxResult resetCommandList()
    {
        command_list_->Reset(command_allocators_[fence_index_], nullptr);
        resetState();   // re-install state
        return kGfxResult_NoError;
    }

    void resetState()
    {
        bound_kernel_ = {};
        descriptor_heap_id_ = 0;
        bound_viewport_.invalidate();
        bound_scissor_rect_.invalidate();
        force_install_index_buffer_ = true;
        force_install_vertex_buffer_ = true;
        force_install_draw_id_buffer_ = true;
        for(auto &bound_color_target : bound_color_targets_)
            bound_color_target = {};
        bound_depth_stencil_target_ = {};
    }

    inline bool isInterop() const { return is_interop_; }

    inline bool isInterop(GfxAccelerationStructure const &acceleration_structure) const
    {
        if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
            return false;   // not a valid acceleration structure object
        AccelerationStructure const &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
        if(!buffer_handles_.has_handle(gfx_acceleration_structure.bvh_buffer_.handle))
            return false;   // not an interop acceleration structure object
        return buffers_[gfx_acceleration_structure.bvh_buffer_].isInterop();
    }

    static inline GfxInternal *GetGfx(GfxContext &context) { return reinterpret_cast<GfxInternal *>(context.handle); }

    static void DispenseDrawState(GfxDrawState &draw_state)
    {
        draw_state.handle = draw_state_handles_.allocate_handle();
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        GFX_ASSERT(!draw_states_.has(draw_state_index));    // should never happen
        draw_states_.insert(draw_state_index).reference_count_ = 1;
    }

    static void RetainDrawState(GfxDrawState const &draw_state)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);  // look up draw state
        GFX_ASSERT(gfx_draw_state != nullptr); if(!gfx_draw_state) return;
        GFX_ASSERT(gfx_draw_state->reference_count_ > 0);
        ++gfx_draw_state->reference_count_;
    }

    static void ReleaseDrawState(GfxDrawState const &draw_state)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);  // look up draw state
        GFX_ASSERT(gfx_draw_state != nullptr); if(!gfx_draw_state) return;
        GFX_ASSERT(gfx_draw_state->reference_count_ > 0);
        if(--gfx_draw_state->reference_count_ == 0)
        {
            draw_states_.erase(draw_state_index);
            draw_state_handles_.free_handle(draw_state.handle);
        }
    }

    static GfxResult SetDrawStateColorTarget(GfxDrawState const &draw_state, uint32_t target_index, DXGI_FORMAT texture_format)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set color target on an invalid draw state object");
        if(target_index >= kGfxConstant_MaxRenderTarget)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot have more than %u render targets in draw state object", (uint32_t)kGfxConstant_MaxRenderTarget);
        gfx_draw_state->draw_state_.color_formats_[target_index] = texture_format;
        return kGfxResult_NoError;
    }

    static GfxResult SetDrawStateDepthStencilTarget(GfxDrawState const &draw_state, DXGI_FORMAT texture_format)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set depth/stencil target on an invalid draw state object");
        gfx_draw_state->draw_state_.depth_stencil_format_ = texture_format;
        return kGfxResult_NoError;
    }

    static GfxResult SetDrawStateCullMode(GfxDrawState const &draw_state, D3D12_CULL_MODE cull_mode)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set cull mode on an invalid draw state object");
        gfx_draw_state->draw_state_.raster_state_.cull_mode_ = cull_mode;
        return kGfxResult_NoError;
    }

    static GfxResult SetDrawStateFillMode(GfxDrawState const &draw_state, D3D12_FILL_MODE fill_mode)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set fill mode on an invalid draw state object");
        gfx_draw_state->draw_state_.raster_state_.fill_mode_ = fill_mode;
        return kGfxResult_NoError;
    }

    static GfxResult SetDrawStateDepthFunction(GfxDrawState const &draw_state, D3D12_COMPARISON_FUNC depth_function)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set depth function on an invalid draw state object");
        gfx_draw_state->draw_state_.depth_stencil_state_.depth_func_ = depth_function;
        return kGfxResult_NoError;
    }

    static GfxResult SetDrawStateDepthWriteMask(GfxDrawState const &draw_state, D3D12_DEPTH_WRITE_MASK depth_write_mask)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set depth write mask on an invalid draw state object");
        gfx_draw_state->draw_state_.depth_stencil_state_.depth_write_mask_ = depth_write_mask;
        return kGfxResult_NoError;
    }

    static GfxResult SetDrawStatePrimitiveTopologyType(GfxDrawState const &draw_state, D3D12_PRIMITIVE_TOPOLOGY_TYPE primitive_topology_type)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set primitive topology type on an invalid draw state object");
        gfx_draw_state->draw_state_.primitive_topology_type_ = primitive_topology_type;
        return kGfxResult_NoError;
    }

    static GfxResult SetDrawStateBlendMode(GfxDrawState const &draw_state, D3D12_BLEND src_blend, D3D12_BLEND dst_blend, D3D12_BLEND_OP blend_op, D3D12_BLEND src_blend_alpha, D3D12_BLEND dst_blend_alpha, D3D12_BLEND_OP blend_op_alpha)
    {
        uint32_t const draw_state_index = static_cast<uint32_t>(draw_state.handle & 0xFFFFFFFFull);
        DrawState *gfx_draw_state = draw_states_.at(draw_state_index);
        if(!gfx_draw_state)
            return GFX_SET_ERROR(kGfxResult_InvalidParameter, "Cannot set blend mode on an invalid draw state object");
        gfx_draw_state->draw_state_.blend_state_.src_blend_ = src_blend;
        gfx_draw_state->draw_state_.blend_state_.dst_blend_ = dst_blend;
        gfx_draw_state->draw_state_.blend_state_.blend_op_ = blend_op;
        gfx_draw_state->draw_state_.blend_state_.src_blend_alpha_ = src_blend_alpha;
        gfx_draw_state->draw_state_.blend_state_.dst_blend_alpha_ = dst_blend_alpha;
        gfx_draw_state->draw_state_.blend_state_.blend_op_alpha_ = blend_op_alpha;
        return kGfxResult_NoError;
    }

private:
    // http://www.cse.yorku.ca/~oz/hash.html
    static uint64_t Hash(char const *value)
    {
        uint64_t hash = 0;
        GFX_ASSERT(value != nullptr);
        if(value == nullptr) return hash;
        while(*value)
        {
            hash = static_cast<uint64_t>(*value) + (hash << 6) + (hash << 16) - hash;
            ++value;
        }
        return hash;
    }

    // https://stackoverflow.com/a/2595226
    template<typename TYPE>
    static void HashCombine(uint64_t &seed, TYPE const &value)
    {
        seed ^= std::hash<TYPE>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    template<typename TYPE>
    static inline void SetObjectName(TYPE &object, char const *name)
    {
        GFX_ASSERT(name != nullptr);
        if(!*name || (object.Object::flags_ & Object::kFlag_Named) != 0)
            return; // no name or already named
        std::vector<WCHAR> buffer(mbstowcs(nullptr, name, 0) + 1);
        mbstowcs(buffer.data(), name, buffer.size());
        object.Object::flags_ |= Object::kFlag_Named;
        GFX_ASSERT(object.resource_ != nullptr);
        object.resource_->SetName(buffer.data());
    }

    static inline void SetDebugName(ID3D12Object *object, char const *debug_name)
    {
        GFX_ASSERT(object != nullptr);
        debug_name = (debug_name ? debug_name : "");
        std::vector<WCHAR> buffer(mbstowcs(nullptr, debug_name, 0) + 1);
        mbstowcs(buffer.data(), debug_name, buffer.size());
        object->SetName(buffer.data());
    }

    static inline D3D12_SHADER_BYTECODE GetShaderBytecode(IDxcBlob *shader_bytecode)
    {
        D3D12_SHADER_BYTECODE result = {};
        if(shader_bytecode)
        {
            result.pShaderBytecode = shader_bytecode->GetBufferPointer();
            result.BytecodeLength = shader_bytecode->GetBufferSize();
        }
        return result;
    }

    static inline uint32_t GetMipLevel(Program::Parameter const &parameter, uint32_t texture_index)
    {
        GFX_ASSERT(parameter.type_ == Program::Parameter::kType_Image);
        if(parameter.type_ != Program::Parameter::kType_Image) return 0;
        GFX_ASSERT(texture_index < parameter.data_.image_.texture_count);
        return (parameter.data_.image_.mip_levels_ != nullptr ?
            parameter.data_.image_.mip_levels_[texture_index] : 0);
    }

    static inline void ResolveClearValueForTexture(Texture &texture, float const *clear_value, DXGI_FORMAT format)
    {
        if(clear_value != nullptr)
            memcpy(texture.clear_value_, clear_value, sizeof(texture.clear_value_));
        else if(IsDepthStencilFormat(format))
        {
            float const default_depth_stencil_clear_value[] =
            {
                1.0f,
                0.0f,
                0.0f,
                0.0f
            };
            memcpy(texture.clear_value_, default_depth_stencil_clear_value, sizeof(texture.clear_value_));
        }
        else
        {
            float const default_color_clear_value[] =
            {
                0.0f,
                0.0f,
                0.0f,
                1.0f
            };
            memcpy(texture.clear_value_, default_color_clear_value, sizeof(texture.clear_value_));
        }
    }

    static inline D3D12_GRAPHICS_PIPELINE_STATE_DESC GetDefaultPSODesc()
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        static_assert(kGfxConstant_MaxRenderTarget <= ARRAYSIZE(pso_desc.BlendState.RenderTarget), "Exceeded maximum number of render targets");
        for(uint32_t i = 0; i < ARRAYSIZE(pso_desc.BlendState.RenderTarget); ++i)
            pso_desc.BlendState.RenderTarget[i] = GetDefaultBlendState();
        pso_desc.SampleMask                            = UINT_MAX;
        pso_desc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode              = D3D12_CULL_MODE_BACK;
        pso_desc.RasterizerState.FrontCounterClockwise = TRUE;
        pso_desc.RasterizerState.DepthClipEnable       = TRUE;
        pso_desc.DepthStencilState.DepthFunc           = D3D12_COMPARISON_FUNC_LESS;
        pso_desc.DepthStencilState.DepthWriteMask      = D3D12_DEPTH_WRITE_MASK_ALL;
        pso_desc.PrimitiveTopologyType                 = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.SampleDesc.Count                      = 1;
        return pso_desc;
    }

    static inline D3D12_RENDER_TARGET_BLEND_DESC GetDefaultBlendState()
    {
        D3D12_RENDER_TARGET_BLEND_DESC
        blend_state                       = {};
        blend_state.SrcBlend              = D3D12_BLEND_ONE;
        blend_state.DestBlend             = D3D12_BLEND_ZERO;
        blend_state.BlendOp               = D3D12_BLEND_OP_ADD;
        blend_state.SrcBlendAlpha         = D3D12_BLEND_ONE;
        blend_state.DestBlendAlpha        = D3D12_BLEND_ZERO;
        blend_state.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        blend_state.LogicOp               = D3D12_LOGIC_OP_NOOP;
        blend_state.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        return blend_state;
    }

    static inline uint32_t GetBytesPerPixel(DXGI_FORMAT format)
    {
        switch(format)
        {
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
            return 1;
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
            return 2;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
            return 4;
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
            return 8;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return 16;
        default:
            break;  // unsupported texture format
        }
        return 0;
    }

    static inline uint32_t GetChannelCount(DXGI_FORMAT format)
    {
        switch(format)
        {
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 1;
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
            return 2;
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
            return 3;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return 4;
        default:
            break;  // unsupported texture format
        }
        return 0;
    }

    static inline bool IsSRGBFormat(DXGI_FORMAT format)
    {
        switch(format)
        {
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return true;
        default:
            break;
        }
        return false;
    }

    static inline bool IsDepthStencilFormat(DXGI_FORMAT format)
    {
        switch(format)
        {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            break;
        }
        return false;
    }

    static inline bool HasStencilBits(DXGI_FORMAT format)
    {
        switch(format)
        {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            break;
        }
        return false;
    }

    static inline DXGI_FORMAT GetUAVFormat(DXGI_FORMAT format)
    {
        switch(format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        default:
            break;
        }
        return GetCBVSRVUAVFormat(format);
    }

    static inline DXGI_FORMAT GetCBVSRVUAVFormat(DXGI_FORMAT format)
    {
        switch(format)
        {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS: // fall through
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:
            break;
        }
        return format;
    }

    // https://stackoverflow.com/questions/41231586/how-to-detect-if-developer-mode-is-active-on-windows-10/41232108#41232108
    static inline bool IsDeveloperModeEnabled()
    {
        HKEY hkey;
        LSTATUS status;
        if(!SUCCEEDED(RegOpenKeyExW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock)", 0, KEY_READ, &hkey)))
            return false;
        DWORD value = {};
        DWORD dword_size = sizeof(value);
        status = RegQueryValueExW(hkey, L"AllowDevelopmentWithoutDevLicense", 0, NULL, reinterpret_cast<LPBYTE>(&value), &dword_size);
        RegCloseKey(hkey);
        if(!SUCCEEDED(status))
            return false;
        return (value != 0 ? true : false);
    }

    static inline D3D12_RESOURCE_STATES GetShaderVisibleResourceState(Kernel const &kernel)
    {
        return kernel.isGraphics() && kernel.ps_reflection_ != nullptr
                 ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
                 : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    uint64_t getDescriptorHeapId() const
    {
        return (static_cast<uint64_t>(descriptors_.descriptor_heap_ != nullptr ? descriptors_.descriptor_heap_->GetDesc().NumDescriptors : 0) << 32) |
               (static_cast<uint64_t>(sampler_descriptors_.descriptor_heap_ != nullptr ? sampler_descriptors_.descriptor_heap_->GetDesc().NumDescriptors : 0));
    }

    template<typename TYPE>
    void collect(TYPE *resource)
    {
        Garbage garbage;
        if(resource == nullptr) return;
        garbage.garbage_[0] = (uintptr_t)resource;
        garbage.garbage_collector_ = Garbage::ResourceCollector<TYPE>;
        garbage_collection_.push_back(std::move(garbage));
    }

    void collect(Buffer const &buffer)
    {
        if(buffer.reference_count_ != nullptr)
        {
            GFX_ASSERT(*buffer.reference_count_ > 0);
            if(--*buffer.reference_count_ > 0)
                return; // still in use
        }
        if(buffer.isInterop() && command_list_ != nullptr && buffer.resource_ != nullptr && buffer.resource_state_ != nullptr && *buffer.resource_state_ != buffer.initial_resource_state_)
        {
            D3D12_RESOURCE_BARRIER resource_barrier = {};
            resource_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            resource_barrier.Transition.pResource = buffer.resource_;
            resource_barrier.Transition.StateBefore = *buffer.resource_state_;
            resource_barrier.Transition.StateAfter = buffer.initial_resource_state_;
            resource_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            command_list_->ResourceBarrier(1, &resource_barrier);
        }
        collect(buffer.resource_);
        collect(buffer.allocation_);
        gfxFree(buffer.resource_state_);
        gfxFree(buffer.reference_count_);
        gfxFree(buffer.transitioned_);
    }

    void collect(Texture const &texture)
    {
        if(texture.isInterop() && command_list_ != nullptr && texture.resource_ != nullptr && texture.resource_state_ != texture.initial_resource_state_)
        {
            D3D12_RESOURCE_BARRIER resource_barrier = {};
            resource_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            resource_barrier.Transition.pResource = texture.resource_;
            resource_barrier.Transition.StateBefore = texture.resource_state_;
            resource_barrier.Transition.StateAfter = texture.initial_resource_state_;
            resource_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            command_list_->ResourceBarrier(1, &resource_barrier);
        }
        collect(texture.resource_);
        collect(texture.allocation_);
        for(uint32_t i = 0; i < ARRAYSIZE(texture.dsv_descriptor_slots_); ++i)
            for(size_t j = 0; j < texture.dsv_descriptor_slots_[i].size(); ++j)
                freeDSVDescriptor(texture.dsv_descriptor_slots_[i][j]);
        for(uint32_t i = 0; i < ARRAYSIZE(texture.rtv_descriptor_slots_); ++i)
            for(size_t j = 0; j < texture.rtv_descriptor_slots_[i].size(); ++j)
                freeRTVDescriptor(texture.rtv_descriptor_slots_[i][j]);
    }

    void collect(SamplerState const &sampler_state)
    {
        freeSamplerDescriptor(sampler_state.descriptor_slot_);
    }

    void collect(AccelerationStructure const &acceleration_structure)
    {
        destroyBuffer(acceleration_structure.bvh_buffer_);
    }

    void collect(RaytracingPrimitive const &raytracing_primitive)
    {
        switch(raytracing_primitive.type_)
        {
        case RaytracingPrimitive::kType_Triangles:
            destroyBuffer(raytracing_primitive.triangles_.bvh_buffer_);
            destroyBuffer(raytracing_primitive.triangles_.index_buffer_);
            destroyBuffer(raytracing_primitive.triangles_.vertex_buffer_);
            break;
        case RaytracingPrimitive::kType_Instance:
            break;  // nothing to collect on instanced primitives
        case RaytracingPrimitive::kType_Procedural:
            destroyBuffer(raytracing_primitive.procedural_.bvh_buffer_);
            destroyBuffer(raytracing_primitive.procedural_.procedural_buffer_);
            break;
        default:
            GFX_ASSERTMSG(0, "An invalid raytracing primitive type was supplied");
            break;  // invalid raytracing primitive type
        }
    }

    void collect(Sbt const &sbt)
    {
        for(uint32_t i = 0; i < kGfxShaderGroupType_Count; ++i)
        {
            destroyBuffer(sbt.sbt_buffers_[i]);
            for(auto &shader_record : sbt.shader_records_[i])
            {
                Sbt::ShaderRecord const &sbt_record = shader_record.second;
                for(auto &parameter : sbt_record.bound_parameters_)
                {
                    freeDescriptor(parameter.descriptor_slot_);
                }
                if(sbt_record.parameters_)
                    for(auto &parameter : *sbt_record.parameters_)
                        parameter.second.unset();   // release parameter resources
            }
        }
    }

    void collect(Program const &program)
    {
        for(Program::Parameters::const_iterator it = program.parameters_.begin(); it != program.parameters_.end(); ++it)
            const_cast<Program::Parameter &>((*it).second).unset(); // release parameter resources
    }

    void collect(Kernel const &kernel)
    {
        gfxFree(kernel.num_threads_);
        collect(kernel.lib_bytecode_);
        collect(kernel.lib_reflection_);
        collect(kernel.root_signature_);
        for(std::map<uint32_t, Kernel::LocalParameter>::const_iterator it = kernel.local_parameters_.begin(); it != kernel.local_parameters_.end(); ++it)
            collect((*it).second.local_root_signature_);
        collect(kernel.pipeline_state_);
        collect(kernel.state_object_);
        for(uint32_t i = 0; i < kernel.parameter_count_; ++i)
        {
            freeDescriptor(kernel.parameters_[i].descriptor_slot_);
            for(uint32_t j = 0; j < kernel.parameters_[i].variable_count_; ++j)
                kernel.parameters_[i].variables_[j].~Variable();
            gfxFree(kernel.parameters_[i].variables_);
            kernel.parameters_[i].~Parameter();
        }
        gfxFree(kernel.parameters_);
    }

    void collect(DescriptorHeap const &descriptor_heap)
    {
        collect(descriptor_heap.descriptor_heap_);
    }

    void collect(uint32_t descriptor_slot, GfxFreelist &descriptor_freelist)
    {
        Garbage garbage;
        if(descriptor_slot == 0xFFFFFFFFu) return;
        garbage.garbage_[0] = (uintptr_t)descriptor_slot;
        garbage.garbage_[1] = (uintptr_t)&descriptor_freelist;
        GFX_ASSERT(descriptor_slot < descriptor_freelist.size());
        garbage.garbage_collector_ = Garbage::DescriptorCollector;
        garbage_collection_.push_back(std::move(garbage));
    }

    GfxResult runGarbageCollection()
    {
        for(uint32_t i = 0; i < garbage_collection_.size();)
            if(++garbage_collection_[i].deletion_counter_ >= max_frames_in_flight_ && !i)
                garbage_collection_.pop_front();    // release
            else
                ++i;
        return kGfxResult_NoError;
    }

    GfxResult forceGarbageCollection()
    {
        while(!garbage_collection_.empty())
        {
            ++garbage_collection_.front().deletion_counter_;
            garbage_collection_.pop_front();    // release
        }
        return kGfxResult_NoError;
    }

    uint32_t allocateDescriptor(uint32_t descriptor_count = 1)
    {
        uint32_t const descriptor_slot = freelist_descriptors_.allocate_slots(descriptor_count);
        uint32_t const size = (descriptors_.descriptor_heap_ != nullptr ? descriptors_.descriptor_heap_->GetDesc().NumDescriptors : 0);
        if(freelist_descriptors_.size() > size)
        {
            ID3D12DescriptorHeap *descriptor_heap = nullptr;
            D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
            descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            descriptor_heap_desc.NumDescriptors = freelist_descriptors_.size();
            descriptor_heap_desc.NumDescriptors += ((descriptor_heap_desc.NumDescriptors + 2) >> 1);
            descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            device_->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&descriptor_heap));
            if(descriptor_heap == nullptr) { freelist_descriptors_.free_slot(descriptor_slot); return 0xFFFFFFFFu; };
            descriptors_.descriptor_handle_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            SetDebugName(descriptor_heap, "gfx_CBVSRVUAVDescriptorHeap");
            collect(descriptors_);  // free previous descriptor heap
            descriptors_.descriptor_heap_ = descriptor_heap;
        }
        return descriptor_slot;
    }

    void freeDescriptor(uint32_t descriptor_slot)
    {
        collect(descriptor_slot, freelist_descriptors_);
    }

    uint32_t allocateDSVDescriptor()
    {
        uint32_t const dsv_slot = freelist_dsv_descriptors_.allocate_slot();
        uint32_t const size = (dsv_descriptors_.descriptor_heap_ != nullptr ? dsv_descriptors_.descriptor_heap_->GetDesc().NumDescriptors : 0);
        if(freelist_dsv_descriptors_.size() > size)
        {
            ID3D12DescriptorHeap *descriptor_heap = nullptr;
            D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
            descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            descriptor_heap_desc.NumDescriptors = freelist_dsv_descriptors_.size();
            descriptor_heap_desc.NumDescriptors += ((descriptor_heap_desc.NumDescriptors + 2) >> 1);
            device_->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&descriptor_heap));
            if(descriptor_heap == nullptr) { freelist_dsv_descriptors_.free_slot(dsv_slot); return 0xFFFFFFFFu; };
            if(size > 0) device_->CopyDescriptorsSimple(size, descriptor_heap->GetCPUDescriptorHandleForHeapStart(),
                dsv_descriptors_.descriptor_heap_->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            dsv_descriptors_.descriptor_handle_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            collect(dsv_descriptors_);  // free previous descriptor heap
            SetDebugName(descriptor_heap, "gfx_DSVDescriptorHeap");
            dsv_descriptors_.descriptor_heap_ = descriptor_heap;
        }
        return dsv_slot;
    }

    void freeDSVDescriptor(uint32_t dsv_slot)
    {
        collect(dsv_slot, freelist_dsv_descriptors_);
    }

    uint32_t allocateRTVDescriptor()
    {
        uint32_t const rtv_slot = freelist_rtv_descriptors_.allocate_slot();
        uint32_t const size = (rtv_descriptors_.descriptor_heap_ != nullptr ? rtv_descriptors_.descriptor_heap_->GetDesc().NumDescriptors : 0);
        if(freelist_rtv_descriptors_.size() > size)
        {
            ID3D12DescriptorHeap *descriptor_heap = nullptr;
            D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
            descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            descriptor_heap_desc.NumDescriptors = freelist_rtv_descriptors_.size();
            descriptor_heap_desc.NumDescriptors += ((descriptor_heap_desc.NumDescriptors + 2) >> 1);
            device_->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&descriptor_heap));
            if(descriptor_heap == nullptr) { freelist_rtv_descriptors_.free_slot(rtv_slot); return 0xFFFFFFFFu; };
            if(size > 0) device_->CopyDescriptorsSimple(size, descriptor_heap->GetCPUDescriptorHandleForHeapStart(),
                rtv_descriptors_.descriptor_heap_->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            rtv_descriptors_.descriptor_handle_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            collect(rtv_descriptors_);  // free previous descriptor heap
            SetDebugName(descriptor_heap, "gfx_RTVDescriptorHeap");
            rtv_descriptors_.descriptor_heap_ = descriptor_heap;
        }
        return rtv_slot;
    }

    void freeRTVDescriptor(uint32_t rtv_slot)
    {
        collect(rtv_slot, freelist_rtv_descriptors_);
    }

    uint32_t allocateSamplerDescriptor()
    {
        uint32_t const sampler_slot = freelist_sampler_descriptors_.allocate_slot();
        uint32_t const size = (sampler_descriptors_.descriptor_heap_ != nullptr ? sampler_descriptors_.descriptor_heap_->GetDesc().NumDescriptors : 0);
        if(freelist_sampler_descriptors_.size() > size)
        {
            ID3D12DescriptorHeap *descriptor_heap = nullptr;
            D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
            descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            descriptor_heap_desc.NumDescriptors = freelist_sampler_descriptors_.size();
            descriptor_heap_desc.NumDescriptors += ((descriptor_heap_desc.NumDescriptors + 2) >> 1);
            descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            device_->CreateDescriptorHeap(&descriptor_heap_desc, IID_PPV_ARGS(&descriptor_heap));
            if(descriptor_heap == nullptr) { freelist_sampler_descriptors_.free_slot(sampler_slot); return 0xFFFFFFFFu; };
            sampler_descriptors_.descriptor_handle_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            uint32_t const dummy_descriptor = (sampler_descriptors_.descriptor_heap_ == nullptr ? sampler_slot : dummy_descriptors_[Kernel::Parameter::kType_Sampler]);
            collect(sampler_descriptors_);  // free previous descriptor heap
            SetDebugName(descriptor_heap, "gfx_SamplerDescriptorHeap");
            sampler_descriptors_.descriptor_heap_ = descriptor_heap;
            GFX_ASSERT(dummy_descriptor != 0xFFFFFFFFu);
            if(dummy_descriptor != 0xFFFFFFFFu)
            {
                D3D12_SAMPLER_DESC sampler_desc = {};
                sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler_desc.MaxLOD = static_cast<float>(D3D12_REQ_MIP_LEVELS);
                device_->CreateSampler(&sampler_desc, sampler_descriptors_.getCPUHandle(dummy_descriptor));
            }
            for(uint32_t i = 0; i < sampler_states_.size(); ++i)
            {
                SamplerState const &sampler_state = sampler_states_.data()[i];
                if(sampler_state.descriptor_slot_ == 0xFFFFFFFFu) continue; // invalid descriptor slot
                device_->CreateSampler(&sampler_state.sampler_desc_, sampler_descriptors_.getCPUHandle(sampler_state.descriptor_slot_));
            }
        }
        return sampler_slot;
    }

    void freeSamplerDescriptor(uint32_t sampler_slot)
    {
        collect(sampler_slot, freelist_sampler_descriptors_);
    }

    GfxResult createRootSignature(Kernel &kernel)
    {
        ID3DBlob *result = nullptr, *error = nullptr;
        struct RootSignatureParameters
        {
            std::vector<Kernel::Parameter> kernel_parameters;
            std::vector<D3D12_ROOT_PARAMETER> root_parameters;
            std::vector<D3D12_DESCRIPTOR_RANGE> descriptor_ranges;
        };
        RootSignatureParameters global_root_signature_parameters;
        std::map<uint64_t, size_t> parameter_id_to_index;
        std::map<uint32_t, RootSignatureParameters> local_root_signatures_parameters;
        std::map<uint32_t, GfxShaderGroupType> local_root_signature_spaces;
        GFX_ASSERT(kernel.root_signature_ == nullptr && kernel.parameters_ == nullptr);

        for(auto &i : kernel.local_root_signature_associations_)
            local_root_signature_spaces[i.second.local_root_signature_space] = i.second.shader_group_type;

        std::vector<void *> allocated_memory;
        struct MemoryReleaser
        {
            GFX_NON_COPYABLE(MemoryReleaser);
            inline MemoryReleaser(std::vector<void *> &allocated_memory) : allocated_memory_(allocated_memory) {}
            inline ~MemoryReleaser() { for(size_t i = 0; i < allocated_memory_.size(); ++i) gfxFree(allocated_memory_[i]); }
            std::vector<void *> allocated_memory_;
        };
        MemoryReleaser const memory_releaser(allocated_memory);

        for(uint32_t i = 0; i < kShaderType_Count; ++i)
        {
            D3D12_ROOT_PARAMETER root_parameter = {};
            ID3D12ShaderReflection *shader = nullptr;
            ID3D12LibraryReflection *library = nullptr;
            switch(i)
            {
            case kShaderType_CS:
                shader = kernel.cs_reflection_;
                break;
            case kShaderType_AS:
                shader = kernel.as_reflection_;
                root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
                break;
            case kShaderType_MS:
                shader = kernel.ms_reflection_;
                root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
                break;
            case kShaderType_VS:
                shader = kernel.vs_reflection_;
                root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
                break;
            case kShaderType_GS:
                shader = kernel.gs_reflection_;
                root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
                break;
            case kShaderType_PS:
                shader = kernel.ps_reflection_;
                root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                break;
            case kShaderType_LIB:
                library = kernel.lib_reflection_;
                root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                break;
            default:
                GFX_ASSERTMSG(0, "Unsupported shader type `%u' was encountered", i);
                break;
            }

            auto process_resource_binding = [&](ID3D12FunctionReflection *function, D3D12_SHADER_INPUT_BIND_DESC &resource_desc)
            {
                Kernel::Parameter kernel_parameter = {};
                D3D12_DESCRIPTOR_RANGE descriptor_range = {};
                kernel_parameter.parameter_id_ = Hash(resource_desc.Name);

                const bool is_local_root_signature_paramter = local_root_signature_spaces.find(resource_desc.Space) != local_root_signature_spaces.end();
                RootSignatureParameters &root_signature_parameters = is_local_root_signature_paramter ?
                    local_root_signatures_parameters[resource_desc.Space] : global_root_signature_parameters;

                auto it = parameter_id_to_index.find(kernel_parameter.parameter_id_);
                if(it != parameter_id_to_index.end())
                {
                    if(root_signature_parameters.root_parameters[it->second].ShaderVisibility == root_parameter.ShaderVisibility)
                    {
                        return; // The parameter already registered by other function, skip
                    }
                }

                switch(resource_desc.Type)
                {
                case D3D_SIT_CBUFFER:
                    {
                        descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                        bool is_root_constant = (strcmp(resource_desc.Name, "$Globals") == 0) || is_local_root_signature_paramter;
                        if(is_root_constant)
                        {
                            ID3D12ShaderReflectionConstantBuffer *constant_buffer = function ? function->GetConstantBufferByName(resource_desc.Name) : shader->GetConstantBufferByName(resource_desc.Name);
                            GFX_ASSERT(constant_buffer != nullptr);
                            D3D12_SHADER_BUFFER_DESC buffer_desc = {};
                            constant_buffer->GetDesc(&buffer_desc);
                            if(buffer_desc.Size > 256 && !is_local_root_signature_paramter)  // https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html#root-argument-limits
                                is_root_constant = false;   // if exceeding the global root parameters size limit, go through the constant buffer pool
                            kernel_parameter.variables_ = (Kernel::Parameter::Variable *)gfxMalloc(buffer_desc.Variables * sizeof(Kernel::Parameter::Variable));
                            allocated_memory.push_back(kernel_parameter.variables_);
                            kernel_parameter.variable_count_ = buffer_desc.Variables;
                            kernel_parameter.variable_size_ = buffer_desc.Size;
                            for(uint32_t k = 0; k < buffer_desc.Variables; ++k)
                            {
                                ID3D12ShaderReflectionVariable *root_variable = constant_buffer->GetVariableByIndex(k);
                                GFX_ASSERT(root_variable != nullptr);
                                D3D12_SHADER_VARIABLE_DESC variable_desc = {};
                                root_variable->GetDesc(&variable_desc);
                                new(&kernel_parameter.variables_[k]) Kernel::Parameter::Variable();
                                kernel_parameter.variables_[k].data_start_ = variable_desc.StartOffset;
                                kernel_parameter.variables_[k].parameter_id_ = Hash(variable_desc.Name);
                                kernel_parameter.variables_[k].data_size_ = variable_desc.Size;
                            }
                        }
                        kernel_parameter.type_ = (is_root_constant ? Kernel::Parameter::kType_Constants : Kernel::Parameter::kType_ConstantBuffer);
                    }
                    break;
                case D3D_SIT_TBUFFER:
                case D3D_SIT_TEXTURE:
                case D3D_SIT_STRUCTURED:
                case D3D_SIT_BYTEADDRESS:
                    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    switch(resource_desc.Dimension)
                    {
                    case D3D_SRV_DIMENSION_BUFFER:
                        kernel_parameter.type_ = Kernel::Parameter::kType_Buffer;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2D:
                        kernel_parameter.type_ = Kernel::Parameter::kType_Texture2D;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
                        kernel_parameter.type_ = Kernel::Parameter::kType_Texture2DArray;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE3D:
                        kernel_parameter.type_ = Kernel::Parameter::kType_Texture3D;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURECUBE:
                        kernel_parameter.type_ = Kernel::Parameter::kType_TextureCube;
                        break;
                    default:
                        GFX_ASSERT(0);  // unimplemented
                        break;
                    }
                    break;
                case D3D_SIT_SAMPLER:
                    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                    kernel_parameter.type_ = Kernel::Parameter::kType_Sampler;
                    break;
                case D3D_SIT_UAV_FEEDBACKTEXTURE:
                    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                    GFX_ASSERT(0);  // unimplemented
                    break;
                case D3D_SIT_UAV_RWTYPED:
                case D3D_SIT_UAV_RWSTRUCTURED:
                case D3D_SIT_UAV_RWBYTEADDRESS:
                case D3D_SIT_UAV_APPEND_STRUCTURED:
                case D3D_SIT_UAV_CONSUME_STRUCTURED:
                case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
                    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                    switch(resource_desc.Dimension)
                    {
                    case D3D_SRV_DIMENSION_BUFFER:
                        kernel_parameter.type_ = Kernel::Parameter::kType_RWBuffer;
                        kernel_parameter.raw_access_ = (resource_desc.Type == D3D_SIT_UAV_RWBYTEADDRESS);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2D:
                        kernel_parameter.type_ = Kernel::Parameter::kType_RWTexture2D;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
                    case D3D_SRV_DIMENSION_TEXTURECUBE: // note: there is no RWTextureCube<> type, RWTexture2DArray<> must be used instead...
                        kernel_parameter.type_ = Kernel::Parameter::kType_RWTexture2DArray;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE3D:
                        kernel_parameter.type_ = Kernel::Parameter::kType_RWTexture3D;
                        break;
                    default:
                        GFX_ASSERT(0);  // unimplemented
                        break;
                    }
                    break;
                case D3D_SIT_RTACCELERATIONSTRUCTURE:
                    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    kernel_parameter.type_ = Kernel::Parameter::kType_AccelerationStructure;
                    break;
                default:
                    GFX_ASSERTMSG(0, "Encountered unsupported shader resource type `%u'", (uint32_t)resource_desc.Type);
                    return;
                }

                descriptor_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                descriptor_range.BaseShaderRegister = resource_desc.BindPoint;
                descriptor_range.NumDescriptors = resource_desc.BindCount;
                descriptor_range.RegisterSpace = resource_desc.Space;

                if(resource_desc.BindCount == 0)
                    switch(kernel_parameter.type_)
                    {
                    case Kernel::Parameter::kType_Buffer:
                    case Kernel::Parameter::kType_RWBuffer:
                    case Kernel::Parameter::kType_Texture2D:
                    case Kernel::Parameter::kType_RWTexture2D:
                    case Kernel::Parameter::kType_Texture2DArray:
                    case Kernel::Parameter::kType_RWTexture2DArray:
                    case Kernel::Parameter::kType_Texture3D:
                    case Kernel::Parameter::kType_RWTexture3D:
                    case Kernel::Parameter::kType_TextureCube:
                        descriptor_range.NumDescriptors = kGfxConstant_NumBindlessSlots;
                        break;
                    default:
                        GFX_PRINT_ERROR(kGfxResult_InternalError, "Bindless is only supported for buffer and texture objets");
                        break;
                    }
                kernel_parameter.descriptor_count_ = descriptor_range.NumDescriptors;

                parameter_id_to_index.insert({kernel_parameter.parameter_id_, root_signature_parameters.kernel_parameters.size()});
                root_signature_parameters.root_parameters.push_back(root_parameter);
                root_signature_parameters.descriptor_ranges.push_back(descriptor_range);
                root_signature_parameters.kernel_parameters.push_back(kernel_parameter);
            };

            if(library != nullptr)
            {
                D3D12_LIBRARY_DESC library_desc;
                library->GetDesc(&library_desc);
                for(uint32_t k = 0; k < library_desc.FunctionCount; k++)
                {
                    ID3D12FunctionReflection *function = library->GetFunctionByIndex(k);
                    D3D12_FUNCTION_DESC function_desc = {};
                    function->GetDesc(&function_desc);
                    for(uint32_t j = 0; j < function_desc.BoundResources; ++j)
                    {
                        D3D12_SHADER_INPUT_BIND_DESC resource_desc = {};
                        function->GetResourceBindingDesc(j, &resource_desc);
                        process_resource_binding(function, resource_desc);
                    }
                }
            }
            else if(shader != nullptr)
            {
                D3D12_SHADER_DESC shader_desc = {};
                shader->GetDesc(&shader_desc);

                for(uint32_t j = 0; j < shader_desc.BoundResources; ++j)
                {
                    D3D12_SHADER_INPUT_BIND_DESC resource_desc = {};
                    shader->GetResourceBindingDesc(j, &resource_desc);
                    process_resource_binding(nullptr, resource_desc);
                }
            }
            else
            {
                continue;
            }

            GFX_ASSERT(global_root_signature_parameters.root_parameters.size() == global_root_signature_parameters.kernel_parameters.size());
            GFX_ASSERT(global_root_signature_parameters.kernel_parameters.size() == global_root_signature_parameters.descriptor_ranges.size());
        }

        auto finalize_root_parameters = [](RootSignatureParameters &root_signature_parameters)
        {
            for(size_t i = 0; i < root_signature_parameters.root_parameters.size(); ++i)
            {
                Kernel::Parameter const &kernel_parameter = root_signature_parameters.kernel_parameters[i];
                if(kernel_parameter.type_ == Kernel::Parameter::kType_Constants)
                {
                    root_signature_parameters.root_parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                    root_signature_parameters.root_parameters[i].Constants.Num32BitValues = kernel_parameter.variable_size_ / sizeof(uint32_t);
                    root_signature_parameters.root_parameters[i].Constants.ShaderRegister = root_signature_parameters.descriptor_ranges[i].BaseShaderRegister;
                    root_signature_parameters.root_parameters[i].Constants.RegisterSpace = root_signature_parameters.descriptor_ranges[i].RegisterSpace;
                }
                else
                {
                    root_signature_parameters.root_parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    root_signature_parameters.root_parameters[i].DescriptorTable.pDescriptorRanges = &root_signature_parameters.descriptor_ranges[i];
                    root_signature_parameters.root_parameters[i].DescriptorTable.NumDescriptorRanges = 1;
                }
            }
        };

        finalize_root_parameters(global_root_signature_parameters);
        for(auto &root_signature_parameters : local_root_signatures_parameters)
            finalize_root_parameters(root_signature_parameters.second);

        D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
        root_signature_desc.pParameters = global_root_signature_parameters.root_parameters.data();
        root_signature_desc.NumParameters = (uint32_t) global_root_signature_parameters.root_parameters.size();
        root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
                                  /*D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                    D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;*/
        root_signature_desc.Flags |= (kernel.vs_bytecode_ != nullptr ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                                                                     : D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS);
        root_signature_desc.Flags |= (kernel.gs_bytecode_ != nullptr ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT
                                                                     : D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);
        if(kernel.ps_bytecode_ == nullptr) root_signature_desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

        D3D12SerializeRootSignature(&root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1, &result, &error);
        if(!result)
        {
            GFX_PRINTLN("Error: Failed to serialize root signature%s%s", error ? ":\r\n" : "", error ? (char const *)error->GetBufferPointer() : "");
            if(error) error->Release();
            return GFX_SET_ERROR(kGfxResult_InternalError, "Failed to serizalize root signature");
        }

        device_->CreateRootSignature(0, result->GetBufferPointer(), result->GetBufferSize(), IID_PPV_ARGS(&kernel.root_signature_));
        if(kernel.root_signature_)
        {
            kernel.parameter_count_ = (uint32_t)global_root_signature_parameters.root_parameters.size();
            kernel.parameters_ = (!global_root_signature_parameters.root_parameters.empty() ? (Kernel::Parameter *)gfxMalloc(kernel.parameter_count_ * sizeof(Kernel::Parameter)) : nullptr);
            GFX_ASSERT(global_root_signature_parameters.root_parameters.size() == global_root_signature_parameters.kernel_parameters.size());
            for(uint32_t i = 0; i < kernel.parameter_count_; ++i)
                new(&kernel.parameters_[i]) Kernel::Parameter(global_root_signature_parameters.kernel_parameters[i]);
        }
        if(error) error->Release();
        result->Release();
        for(auto &root_signature_parameters : local_root_signatures_parameters)
        {
            uint32_t const space = root_signature_parameters.first;
            auto &local_root_signature_parameters = root_signature_parameters.second;

            D3D12_ROOT_SIGNATURE_DESC root_signature_desc2 = {};
            root_signature_desc2.pParameters = local_root_signature_parameters.root_parameters.data();
            root_signature_desc2.NumParameters = (uint32_t) local_root_signature_parameters.root_parameters.size();
            root_signature_desc2.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

            D3D12SerializeRootSignature(&root_signature_desc2, D3D_ROOT_SIGNATURE_VERSION_1, &result, &error);
            if(!result)
            {
                GFX_PRINTLN("Error: Failed to serialize local root signature%s%s", error ? ":\r\n" : "", error ? (char const *)error->GetBufferPointer() : "");
                if(error) error->Release();
                return GFX_SET_ERROR(kGfxResult_InternalError, "Failed to serizalize root signature");
            }

            auto &local_parameters = kernel.local_parameters_[space];
            device_->CreateRootSignature(0, result->GetBufferPointer(), result->GetBufferSize(), IID_PPV_ARGS(&local_parameters.local_root_signature_));
            if(local_parameters.local_root_signature_)
            {
                local_parameters.parameters_ = std::move(local_root_signature_parameters.kernel_parameters);
            }
            if(error) error->Release();
            result->Release();

            size_t local_root_signature_parameters_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
            for(size_t i = 0; i < local_parameters.parameters_.size(); ++i)
            {
                Kernel::Parameter const &kernel_parameter = local_parameters.parameters_[i];
                if(kernel_parameter.type_ == Kernel::Parameter::kType_Constants)
                {
                    local_root_signature_parameters_size += GFX_ALIGN(kernel_parameter.variable_size_, sizeof(D3D12_GPU_VIRTUAL_ADDRESS));
                    local_root_signature_parameters.root_parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                }
                else
                {
                    local_root_signature_parameters_size += sizeof(D3D12_GPU_DESCRIPTOR_HANDLE);
                    break;
                }
            }
            GfxShaderGroupType const shader_group_type = local_root_signature_spaces[space];
            kernel.sbt_record_stride_[shader_group_type] = GFX_MAX(kernel.sbt_record_stride_[shader_group_type], GFX_ALIGN(local_root_signature_parameters_size, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT ));
        }

        return kGfxResult_NoError;
    }

    GfxResult createMeshPipelineState(Kernel &kernel, DrawState::Data const &draw_state)
    {
        GFX_ASSERT(kernel.pipeline_state_ == nullptr);
        if(kernel.root_signature_ == nullptr) return kGfxResult_NoError;
        struct D3D12_MESH_PIPELINE_STATE_DESC
        {
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;        ID3D12RootSignature           *pRootSignature;       } RootSignature;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;                    D3D12_SHADER_BYTECODE         AS;                    } AS;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;                    D3D12_SHADER_BYTECODE         MS;                    } MS;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;                    D3D12_SHADER_BYTECODE         PS;                    } PS;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;                 D3D12_BLEND_DESC              BlendState;            } BlendState;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK;           UINT                          SampleMask;            } SampleMask;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;            D3D12_RASTERIZER_DESC         RasterizerState;       } RasterizerState;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;         D3D12_DEPTH_STENCIL_DESC      DepthStencilState;     } DepthStencilState;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;    D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType; } PrimitiveTopologyType;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS; D3D12_RT_FORMAT_ARRAY         RTVFormats;            } RTVFormats;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT;  DXGI_FORMAT                   DSVFormat;             } DSVFormat;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC;           DXGI_SAMPLE_DESC              SampleDesc;            } SampleDesc;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK;             UINT                          NodeMask;              } NodeMask;
            struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE const Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS;                 D3D12_PIPELINE_STATE_FLAGS    Flags;                 } Flags;
        };
        D3D12_MESH_PIPELINE_STATE_DESC
        pso_desc                                                       = {};
        pso_desc.RootSignature.pRootSignature                          = kernel.root_signature_;
        pso_desc.AS.AS                                                 = GetShaderBytecode(kernel.as_bytecode_);
        pso_desc.MS.MS                                                 = GetShaderBytecode(kernel.ms_bytecode_);
        pso_desc.PS.PS                                                 = GetShaderBytecode(kernel.ps_bytecode_);
        pso_desc.SampleMask.SampleMask                                 = UINT_MAX;
        pso_desc.RasterizerState.RasterizerState.FillMode              = draw_state.raster_state_.fill_mode_;
        pso_desc.RasterizerState.RasterizerState.CullMode              = draw_state.raster_state_.cull_mode_;
        pso_desc.RasterizerState.RasterizerState.FrontCounterClockwise = TRUE;
        pso_desc.RasterizerState.RasterizerState.DepthClipEnable       = TRUE;
        pso_desc.DepthStencilState.DepthStencilState.DepthFunc         = D3D12_COMPARISON_FUNC_LESS;
        pso_desc.DepthStencilState.DepthStencilState.DepthWriteMask    = D3D12_DEPTH_WRITE_MASK_ALL;
        pso_desc.PrimitiveTopologyType.PrimitiveTopologyType           = draw_state.primitive_topology_type_;
        pso_desc.SampleDesc.SampleDesc.Count                           = 1;
        for(uint32_t i = 0; i < ARRAYSIZE(pso_desc.BlendState.BlendState.RenderTarget); ++i)
            pso_desc.BlendState.BlendState.RenderTarget[i] = GetDefaultBlendState();
        {
            for(uint32_t i = 0; i < ARRAYSIZE(draw_state.color_formats_); ++i)
                if(draw_state.color_formats_[i] == DXGI_FORMAT_UNKNOWN)
                    continue;   // no valid color target at index
                else
                {
                    pso_desc.RTVFormats.RTVFormats.RTFormats[i]     = draw_state.color_formats_[i];
                    pso_desc.RTVFormats.RTVFormats.NumRenderTargets = i + 1;
                }
            if(draw_state.depth_stencil_format_ != DXGI_FORMAT_UNKNOWN)
            {
                pso_desc.DepthStencilState.DepthStencilState.DepthEnable    = TRUE;
                pso_desc.DepthStencilState.DepthStencilState.DepthWriteMask = draw_state.depth_stencil_state_.depth_write_mask_;
                pso_desc.DepthStencilState.DepthStencilState.DepthFunc      = draw_state.depth_stencil_state_.depth_func_;
                pso_desc.DSVFormat.DSVFormat                                = draw_state.depth_stencil_format_;
            }
            else if(pso_desc.RTVFormats.RTVFormats.NumRenderTargets == 0)   // special case - if no color target is supplied, draw to back buffer
            {
                if(isInterop())
                    return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to backbuffer when using an interop context");
                pso_desc.RTVFormats.RTVFormats.RTFormats[0]     = back_buffer_format_;
                pso_desc.RTVFormats.RTVFormats.NumRenderTargets = 1;
            }
        }
        if(draw_state.blend_state_)
        {
            pso_desc.BlendState.BlendState.RenderTarget->BlendEnable    = TRUE;
            pso_desc.BlendState.BlendState.RenderTarget->SrcBlend       = draw_state.blend_state_.src_blend_;
            pso_desc.BlendState.BlendState.RenderTarget->DestBlend      = draw_state.blend_state_.dst_blend_;
            pso_desc.BlendState.BlendState.RenderTarget->BlendOp        = draw_state.blend_state_.blend_op_;
            pso_desc.BlendState.BlendState.RenderTarget->SrcBlendAlpha  = draw_state.blend_state_.src_blend_alpha_;
            pso_desc.BlendState.BlendState.RenderTarget->DestBlendAlpha = draw_state.blend_state_.dst_blend_alpha_;
            pso_desc.BlendState.BlendState.RenderTarget->BlendOpAlpha   = draw_state.blend_state_.blend_op_alpha_;
        }
        D3D12_PIPELINE_STATE_STREAM_DESC
        pso_stream_desc                               = {};
        pso_stream_desc.SizeInBytes                   = sizeof(pso_desc);
        pso_stream_desc.pPipelineStateSubobjectStream = &pso_desc;
        mesh_device_->CreatePipelineState(&pso_stream_desc, IID_PPV_ARGS(&kernel.pipeline_state_));
        return kGfxResult_NoError;
    }

    GfxResult createComputePipelineState(Kernel &kernel)
    {
        GFX_ASSERT(kernel.pipeline_state_ == nullptr);
        if(kernel.root_signature_ == nullptr) return kGfxResult_NoError;
        D3D12_COMPUTE_PIPELINE_STATE_DESC
        pso_desc                = {};
        pso_desc.pRootSignature = kernel.root_signature_;
        pso_desc.CS             = GetShaderBytecode(kernel.cs_bytecode_);
        device_->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&kernel.pipeline_state_));
        return kGfxResult_NoError;
    }

    GfxResult createGraphicsPipelineState(Kernel &kernel, DrawState::Data const &draw_state)
    {
        GFX_ASSERT(kernel.pipeline_state_ == nullptr);
        std::vector<D3D12_INPUT_ELEMENT_DESC> input_layout;
        if(kernel.root_signature_ == nullptr) return kGfxResult_NoError;
        if(kernel.vs_reflection_ != nullptr)
        {
            D3D12_SHADER_DESC shader_desc = {};
            GFX_ASSERT(kernel.vertex_stride_ == 0);
            kernel.vs_reflection_->GetDesc(&shader_desc);
            for(uint32_t i = 0; i < shader_desc.InputParameters; ++i)
            {
                uint32_t component_count = 0;
                DXGI_FORMAT component_format = DXGI_FORMAT_UNKNOWN;
                D3D12_SIGNATURE_PARAMETER_DESC parameter_desc = {};
                kernel.vs_reflection_->GetInputParameterDesc(i, &parameter_desc);
                if(parameter_desc.SystemValueType != D3D_NAME_UNDEFINED)
                    continue;   // ignore system value types
                while(parameter_desc.Mask)
                {
                    ++component_count;
                    parameter_desc.Mask >>= 1;
                }
                switch(parameter_desc.ComponentType)
                {
                case D3D_REGISTER_COMPONENT_UINT32:
                    component_format = (component_count == 1 ? DXGI_FORMAT_R32_UINT       :
                                        component_count == 2 ? DXGI_FORMAT_R32G32_UINT    :
                                        component_count == 3 ? DXGI_FORMAT_R32G32B32_UINT :
                                                               DXGI_FORMAT_R32G32B32A32_UINT);
                    break;
                case D3D_REGISTER_COMPONENT_SINT32:
                    component_format = (component_count == 1 ? DXGI_FORMAT_R32_SINT       :
                                        component_count == 2 ? DXGI_FORMAT_R32G32_SINT    :
                                        component_count == 3 ? DXGI_FORMAT_R32G32B32_SINT :
                                                               DXGI_FORMAT_R32G32B32A32_SINT);
                    break;
                case D3D_REGISTER_COMPONENT_FLOAT32:
                    component_format = (component_count == 1 ? DXGI_FORMAT_R32_FLOAT       :
                                        component_count == 2 ? DXGI_FORMAT_R32G32_FLOAT    :
                                        component_count == 3 ? DXGI_FORMAT_R32G32B32_FLOAT :
                                                               DXGI_FORMAT_R32G32B32A32_FLOAT);
                    break;
                default:
                    break;  // D3D_REGISTER_COMPONENT_UNKNOWN
                }
                D3D12_INPUT_ELEMENT_DESC
                input_desc                   = {};
                input_desc.SemanticName      = parameter_desc.SemanticName;
                input_desc.SemanticIndex     = parameter_desc.SemanticIndex;
                input_desc.Format            = component_format;
                input_desc.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
                if(!!strcmp(parameter_desc.SemanticName, "gfx_DrawID"))
                    kernel.vertex_stride_ += component_count * sizeof(uint32_t);
                else
                {
                    input_desc.InputSlot            = 1;
                    input_desc.InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                    input_desc.InstanceDataStepRate = 1;
                }
                input_layout.push_back(input_desc);
            }
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC
        pso_desc                                = GetDefaultPSODesc();
        pso_desc.pRootSignature                 = kernel.root_signature_;
        pso_desc.VS                             = GetShaderBytecode(kernel.vs_bytecode_);
        pso_desc.PS                             = GetShaderBytecode(kernel.ps_bytecode_);
        pso_desc.GS                             = GetShaderBytecode(kernel.gs_bytecode_);
        pso_desc.RasterizerState.CullMode       = draw_state.raster_state_.cull_mode_;
        pso_desc.RasterizerState.FillMode       = draw_state.raster_state_.fill_mode_;
        pso_desc.InputLayout.pInputElementDescs = input_layout.data();
        pso_desc.InputLayout.NumElements        = (uint32_t)input_layout.size();
        pso_desc.PrimitiveTopologyType          = draw_state.primitive_topology_type_;
        {
            for(uint32_t i = 0; i < ARRAYSIZE(draw_state.color_formats_); ++i)
                if(draw_state.color_formats_[i] == DXGI_FORMAT_UNKNOWN)
                    continue;   // no valid color target at index
                else
                {
                    pso_desc.RTVFormats[i]    = draw_state.color_formats_[i];
                    pso_desc.NumRenderTargets = i + 1;
                }
            if(draw_state.depth_stencil_format_ != DXGI_FORMAT_UNKNOWN)
            {
                pso_desc.DepthStencilState.DepthEnable    = TRUE;
                pso_desc.DepthStencilState.DepthWriteMask = draw_state.depth_stencil_state_.depth_write_mask_;
                pso_desc.DepthStencilState.DepthFunc      = draw_state.depth_stencil_state_.depth_func_;
                pso_desc.DSVFormat                        = draw_state.depth_stencil_format_;
            }
            else if(pso_desc.NumRenderTargets == 0)  // special case - if no color target is supplied, draw to back buffer
            {
                if(isInterop())
                    return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to backbuffer when using an interop context");
                pso_desc.RTVFormats[0]    = back_buffer_format_;
                pso_desc.NumRenderTargets = 1;
            }
        }
        if(draw_state.blend_state_)
        {
            pso_desc.BlendState.RenderTarget->BlendEnable    = TRUE;
            pso_desc.BlendState.RenderTarget->SrcBlend       = draw_state.blend_state_.src_blend_;
            pso_desc.BlendState.RenderTarget->DestBlend      = draw_state.blend_state_.dst_blend_;
            pso_desc.BlendState.RenderTarget->BlendOp        = draw_state.blend_state_.blend_op_;
            pso_desc.BlendState.RenderTarget->SrcBlendAlpha  = draw_state.blend_state_.src_blend_alpha_;
            pso_desc.BlendState.RenderTarget->DestBlendAlpha = draw_state.blend_state_.dst_blend_alpha_;
            pso_desc.BlendState.RenderTarget->BlendOpAlpha   = draw_state.blend_state_.blend_op_alpha_;
        }
        device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&kernel.pipeline_state_));
        return kGfxResult_NoError;
    }

    GfxResult createRaytracingPipelineState(Kernel &kernel)
    {
        GFX_ASSERT(kernel.state_object_ == nullptr);
        D3D12_GLOBAL_ROOT_SIGNATURE
        global_root_signature = { kernel.root_signature_ };
        std::vector<D3D12_EXPORT_DESC> export_descs;
        std::vector<std::wstring> exports;
        size_t max_export_length = 0;
        for(size_t i = 0; i < kernel.exports_.size(); ++i)
            max_export_length = GFX_MAX(max_export_length, strlen(kernel.exports_[i].c_str()));
        for(size_t i = 0; i < kernel.subobjects_.size(); ++i)
            max_export_length = GFX_MAX(max_export_length, strlen(kernel.subobjects_[i].c_str()));
        max_export_length += 1;
        std::vector<char> lib_export(max_export_length);
        std::vector<WCHAR> wexport(max_export_length);
        for(size_t i = 0; i < kernel.exports_.size(); ++i)
        {
            GFX_SNPRINTF(lib_export.data(), max_export_length, "%s", kernel.exports_[i].c_str());
            mbstowcs(wexport.data(), lib_export.data(), max_export_length);
            exports.push_back(wexport.data());
        }
        for(size_t i = 0; i < kernel.subobjects_.size(); ++i)
        {
            GFX_SNPRINTF(lib_export.data(), max_export_length, "%s", kernel.subobjects_[i].c_str());
            mbstowcs(wexport.data(), lib_export.data(), max_export_length);
            exports.push_back(wexport.data());
        }
        for(size_t i = 0; i < exports.size(); ++i)
        {
            export_descs.push_back({ exports[i].c_str(), nullptr, D3D12_EXPORT_FLAG_NONE});
        }
        D3D12_DXIL_LIBRARY_DESC
        lib_desc = { GetShaderBytecode(kernel.lib_bytecode_), (UINT)export_descs.size(), export_descs.data()};
        std::vector<D3D12_STATE_SUBOBJECT> subobjects;
        subobjects.reserve(kernel.local_root_signature_associations_.size() + 2);
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_root_signature});
        subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib_desc });
        std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> local_root_signature_associations;
        std::vector<D3D12_LOCAL_ROOT_SIGNATURE> local_root_signatures;
        std::vector<LPCWSTR> local_root_signature_associated_exports;
        local_root_signature_associations.reserve(kernel.local_root_signature_associations_.size());
        local_root_signatures.reserve(kernel.local_root_signature_associations_.size());
        local_root_signature_associated_exports.reserve(kernel.local_root_signature_associations_.size());
        for(auto &i : kernel.local_root_signature_associations_)
        {
            auto &local_parameters = kernel.local_parameters_[i.second.local_root_signature_space];
            local_root_signatures.push_back({local_parameters.local_root_signature_});
            subobjects.push_back({D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local_root_signatures.back()});
            local_root_signature_associated_exports.push_back({i.first.c_str()});
            local_root_signature_associations.push_back({&subobjects.back(), 1, &local_root_signature_associated_exports.back()});
        }
        D3D12_STATE_OBJECT_DESC
        so_desc               = {};
        so_desc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        so_desc.NumSubobjects = (UINT)subobjects.size();
        so_desc.pSubobjects   = subobjects.data();
        dxr_device_->CreateStateObject(&so_desc, IID_PPV_ARGS(&kernel.state_object_));
        return kGfxResult_NoError;
    }

    GfxResult installShaderState(Kernel &kernel, bool indexed = false, Sbt *sbt = nullptr)
    {
        uint32_t root_constants[64];
        bool const is_compute = (kernel.isCompute() || kernel.isRaytracing());
        if(!program_handles_.has_handle(kernel.program_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot %s with a %s kernel pointing to an invalid program object",
                is_compute ? "dispatch" : "draw", is_compute ? "compute" : "graphics");
        if(!is_compute)
        {
            uint32_t color_target_count = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_target = {};
            uint32_t render_width = 0xFFFFFFFFu, render_height = 0xFFFFFFFFu;
            D3D12_CPU_DESCRIPTOR_HANDLE color_targets[kGfxConstant_MaxRenderTarget] = {};
            for(uint32_t i = 0; i < ARRAYSIZE(kernel.draw_state_.color_formats_); ++i)
                if(!bound_color_targets_[i].texture_)
                {
                    if(kernel.draw_state_.color_formats_[i] != DXGI_FORMAT_UNKNOWN)
                        return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to an missing texture object; found at color target %u", i);
                }
                else if(kernel.draw_state_.color_formats_[i] != DXGI_FORMAT_UNKNOWN)
                {
                     // valid bound color target and draw state requires one
                    GfxTexture const &texture = bound_color_targets_[i].texture_;
                    if(!texture_handles_.has_handle(texture.handle))
                        return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to an invalid texture object; found at color target %u", i);
                    Texture &gfx_texture = textures_[texture]; SetObjectName(gfx_texture, texture.name);
                    GFX_TRY(ensureTextureHasRenderTargetView(texture, gfx_texture, bound_color_targets_[i].mip_level_, bound_color_targets_[i].slice_));
                    GFX_ASSERT(gfx_texture.rtv_descriptor_slots_[bound_color_targets_[i].mip_level_][bound_color_targets_[i].slice_] != 0xFFFFFFFFu);
                    color_targets[i] = rtv_descriptors_.getCPUHandle(gfx_texture.rtv_descriptor_slots_[bound_color_targets_[i].mip_level_][bound_color_targets_[i].slice_]);
                    for(uint32_t j = color_target_count; j < i; ++j) color_targets[j] = rtv_descriptors_.getCPUHandle(dummy_rtv_descriptor_);
                    uint32_t const texture_width  = ((gfx_texture.flags_ & Texture::kFlag_AutoResize) != 0 ? window_width_  : texture.width);
                    uint32_t const texture_height = ((gfx_texture.flags_ & Texture::kFlag_AutoResize) != 0 ? window_height_ : texture.height);
                    render_width = GFX_MIN(render_width, texture_width); render_height = GFX_MIN(render_height, texture_height);
                    transitionResource(gfx_texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    color_target_count = i + 1;
                }
            if(!bound_depth_stencil_target_.texture_)
            {
                if(kernel.draw_state_.depth_stencil_format_ != DXGI_FORMAT_UNKNOWN)
                    return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to an missing texture object; found at depth/stencil target");
            }
            else if(kernel.draw_state_.depth_stencil_format_ != DXGI_FORMAT_UNKNOWN)
            {
                GfxTexture const &texture = bound_depth_stencil_target_.texture_;
                if(!texture_handles_.has_handle(texture.handle))
                    return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot draw to an invalid texture object; found at depth/stencil target");
                Texture &gfx_texture = textures_[texture]; SetObjectName(gfx_texture, texture.name);
                GFX_TRY(ensureTextureHasDepthStencilView(texture, gfx_texture, bound_depth_stencil_target_.mip_level_, bound_depth_stencil_target_.slice_));
                GFX_ASSERT(gfx_texture.dsv_descriptor_slots_[bound_depth_stencil_target_.mip_level_][bound_depth_stencil_target_.slice_] != 0xFFFFFFFFu);
                depth_stencil_target = dsv_descriptors_.getCPUHandle(gfx_texture.dsv_descriptor_slots_[bound_depth_stencil_target_.mip_level_][bound_depth_stencil_target_.slice_]);
                uint32_t const texture_width  = ((gfx_texture.flags_ & Texture::kFlag_AutoResize) != 0 ? window_width_  : texture.width);
                uint32_t const texture_height = ((gfx_texture.flags_ & Texture::kFlag_AutoResize) != 0 ? window_height_ : texture.height);
                render_width = GFX_MIN(render_width, texture_width); render_height = GFX_MIN(render_height, texture_height);
                transitionResource(gfx_texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
            if(color_target_count == 0 && depth_stencil_target.ptr == 0)    // special case - if no color target is supplied, draw to back buffer
            {
                render_width = window_width_; render_height = window_height_;
                color_targets[color_target_count++] = rtv_descriptors_.getCPUHandle(back_buffer_rtvs_[fence_index_]);
            }
            D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)render_width, (float)render_height, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
            if(viewport_.x_ != 0.0f || viewport_.y_ != 0.0f || viewport_.width_ != 0.0f || viewport_.height_ != 0.0f)
            {
                viewport.TopLeftX = (float)viewport_.x_;
                viewport.TopLeftY = (float)viewport_.y_;
                viewport.Width    = (float)viewport_.width_;
                viewport.Height   = (float)viewport_.height_;
            }
            if(bound_viewport_ != viewport)
            {
                bound_viewport_ = viewport;
                command_list_->RSSetViewports(1, &viewport);
            }
            D3D12_RECT scissor_rect = { 0, 0, (LONG)render_width, (LONG)render_height };
            if(scissor_rect_.x_ != 0 || scissor_rect_.y_ != 0 || scissor_rect_.width_ != 0 || scissor_rect_.height_ != 0)
            {
                scissor_rect.left   = (LONG)scissor_rect_.x_;
                scissor_rect.top    = (LONG)scissor_rect_.y_;
                scissor_rect.right  = scissor_rect.left + (LONG)scissor_rect_.width_;
                scissor_rect.bottom = scissor_rect.top  + (LONG)scissor_rect_.height_;
            }
            if(bound_scissor_rect_ != scissor_rect)
            {
                bound_scissor_rect_ = scissor_rect;
                command_list_->RSSetScissorRects(1, &scissor_rect);
            }
            command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            command_list_->OMSetRenderTargets(color_target_count, color_targets, false, depth_stencil_target.ptr != 0 ? &depth_stencil_target : nullptr);
        }
        uint64_t const previous_descriptor_heap_id = getDescriptorHeapId();
        Program const &program = programs_[kernel.program_];
        for(uint32_t i = 0; i < kernel.parameter_count_; ++i)
        {
            Kernel::Parameter &parameter = kernel.parameters_[i];
            if(parameter.type_ >= Kernel::Parameter::kType_Count) continue;
            switch(parameter.type_)
            {
            case Kernel::Parameter::kType_Constants:
                populateRootConstants(program, program.parameters_, parameter, root_constants);
                if(is_compute)
                    command_list_->SetComputeRoot32BitConstants(i, parameter.variable_size_ / sizeof(uint32_t), root_constants, 0);
                else
                    command_list_->SetGraphicsRoot32BitConstants(i, parameter.variable_size_ / sizeof(uint32_t), root_constants, 0);
                break;
            case Kernel::Parameter::kType_ConstantBuffer:
                if(parameter.parameter_ == nullptr && parameter.variable_size_ == 0)
                {
                    Program::Parameters::const_iterator const it = program.parameters_.find(parameter.parameter_id_);
                    if(it != program.parameters_.end()) parameter.parameter_ = &(*it).second;
                }
                if(parameter.parameter_ != nullptr || parameter.variable_size_ > 0)
                {
                    freeDescriptor(parameter.descriptor_slot_);
                    parameter.descriptor_slot_ = allocateDescriptor();
                }
                break;
            case Kernel::Parameter::kType_Sampler:
                if(parameter.parameter_ == nullptr)
                {
                    Program::Parameters::const_iterator const it = program.parameters_.find(parameter.parameter_id_);
                    if(it != program.parameters_.end()) parameter.parameter_ = &(*it).second;
                }
                break;
            default:
                if(parameter.parameter_ == nullptr)
                {
                    Program::Parameters::const_iterator const it = program.parameters_.find(parameter.parameter_id_);
                    if(it != program.parameters_.end()) parameter.parameter_ = &(*it).second;
                }
                switch(parameter.type_)
                {
                case Kernel::Parameter::kType_RWTexture2D:
                case Kernel::Parameter::kType_RWTexture3D:
                case Kernel::Parameter::kType_RWTexture2DArray:
                    if(parameter.parameter_ != nullptr && parameter.id_ != parameter.parameter_->id_ && parameter.parameter_->type_ == Program::Parameter::kType_Image)
                        for(uint32_t j = 0; j < parameter.parameter_->data_.image_.texture_count; ++j)
                            if(texture_handles_.has_handle(parameter.parameter_->data_.image_.textures_[j].handle))
                                ensureTextureHasUsageFlag(textures_[parameter.parameter_->data_.image_.textures_[j]], D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                    break;
                case Kernel::Parameter::kType_AccelerationStructure:
                    if(parameter.parameter_ != nullptr && parameter.id_ == parameter.parameter_->id_)
                    {
                        if(acceleration_structure_handles_.has_handle(parameter.parameter_->data_.acceleration_structure_.bvh_.handle))
                        {
                            AccelerationStructure const &acceleration_structure = acceleration_structures_[parameter.parameter_->data_.acceleration_structure_.bvh_];
                            if(acceleration_structure.bvh_buffer_.handle != parameter.parameter_->data_.acceleration_structure_.bvh_buffer_.handle)
                                ++const_cast<Program::Parameter *>(parameter.parameter_)->id_;
                        }
                    }
                    break;
                default:
                    break;
                }
                if(parameter.parameter_ != nullptr && parameter.id_ != parameter.parameter_->id_)
                {
                    freeDescriptor(parameter.descriptor_slot_);
                    GFX_ASSERT(parameter.descriptor_count_ > 0);
                    parameter.descriptor_slot_ = allocateDescriptor(parameter.descriptor_count_);
                }
                break;
            }
        }
        if(sbt)
        {
            bool const invalidate_sbt_descriptors = sbt->descriptor_heap_id_ != previous_descriptor_heap_id;
            bool const invalidate_sbt_parameters = sbt->kernel_ != bound_kernel_;
            sbt->descriptor_heap_id_ = previous_descriptor_heap_id; // update descriptor heap id
            for(uint32_t i = 0; i < kGfxShaderGroupType_Count; ++i)
            {
                auto last_record_it = sbt->shader_records_[i].rbegin();
                if(last_record_it == sbt->shader_records_[i].rend()) continue;
                for(auto &shader_record : sbt->shader_records_[i])
                {
                    Sbt::ShaderRecord &sbt_record = shader_record.second;
                    if(sbt_record.id_ == sbt_record.commited_id_ && !invalidate_sbt_descriptors && !invalidate_sbt_parameters) continue;
                    auto local_root_signature_association = kernel.local_root_signature_associations_.find(sbt_record.shader_identifier_);
                    if(local_root_signature_association != kernel.local_root_signature_associations_.end())
                    {
                        if(invalidate_sbt_parameters)
                        {
                            for(auto &bound_parameter : sbt_record.bound_parameters_)
                            {
                                freeDescriptor(bound_parameter.descriptor_slot_);
                            }
                            sbt_record.bound_parameters_ = kernel.local_parameters_[local_root_signature_association->second.local_root_signature_space].parameters_;
                        }
                        for(auto &parameter : sbt_record.bound_parameters_)
                        {
                            if(parameter.type_ >= Kernel::Parameter::kType_Count) continue;
                            switch(parameter.type_)
                            {
                            case Kernel::Parameter::kType_Constants:
                            case Kernel::Parameter::kType_ConstantBuffer:
                                break;
                            default:
                                if(parameter.parameter_ == nullptr)
                                {
                                    Program::Parameters::const_iterator const it = sbt_record.parameters_->find(parameter.parameter_id_);
                                    if(it != sbt_record.parameters_->end()) parameter.parameter_ = &(*it).second;
                                }
                                if(parameter.parameter_ != nullptr && parameter.id_ != parameter.parameter_->id_)
                                {
                                    freeDescriptor(parameter.descriptor_slot_);
                                    GFX_ASSERT(parameter.descriptor_count_ > 0);
                                    parameter.descriptor_slot_ = allocateDescriptor(parameter.descriptor_count_);
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        uint64_t const descriptor_heap_id = getDescriptorHeapId();
        if(descriptor_heap_id_ != descriptor_heap_id)
        {
            uint32_t descriptor_heap_count = 0;
            ID3D12DescriptorHeap *const descriptor_heaps[] =
            {
                descriptors_.descriptor_heap_,
                sampler_descriptors_.descriptor_heap_
            };
            for(uint32_t i = 0; i < ARRAYSIZE(descriptor_heaps); ++i)
                descriptor_heap_count += (descriptor_heaps[descriptor_heap_count] != nullptr ? 1 : 0);
            command_list_->SetDescriptorHeaps(descriptor_heap_count, descriptor_heaps);
            descriptor_heap_id_ = descriptor_heap_id;
        }
        if(descriptor_heap_id != previous_descriptor_heap_id) populateDummyDescriptors();
        bool const invalidate_descriptors = (kernel.descriptor_heap_id_ != descriptor_heap_id);
        kernel.descriptor_heap_id_ = descriptor_heap_id;    // update descriptor heap id
        for(uint32_t i = 0; i < kernel.parameter_count_; ++i)
        {
            Kernel::Parameter &parameter = kernel.parameters_[i];
            if(parameter.type_ >= Kernel::Parameter::kType_Count) continue;
            if(parameter.type_ == Kernel::Parameter::kType_Constants) continue;
            if(parameter.type_ == Kernel::Parameter::kType_Sampler)
            {
                uint32_t descriptor_slot = dummy_descriptors_[parameter.type_];
                if(parameter.parameter_ != nullptr)
                {
                    if(parameter.parameter_->type_ != Program::Parameter::kType_SamplerState)
                    {
                        if(parameter.id_ != parameter.parameter_->id_)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a sampler state object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                    }
                    else if(!sampler_state_handles_.has_handle(parameter.parameter_->data_.sampler_state_.handle))
                    {
                        if(parameter.parameter_->data_.sampler_state_.handle != 0)
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid sampler state object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                    }
                    else
                    {
                        SamplerState const &sampler_state = sampler_states_[parameter.parameter_->data_.sampler_state_];
                        GFX_ASSERT(sampler_state.descriptor_slot_ != 0xFFFFFFFFu);
                        if(sampler_state.descriptor_slot_ != 0xFFFFFFFFu)
                            descriptor_slot = sampler_state.descriptor_slot_;
                    }
                    parameter.id_ = parameter.parameter_->id_;
                }
                GFX_ASSERT(descriptor_slot < (sampler_descriptors_.descriptor_heap_ != nullptr ? sampler_descriptors_.descriptor_heap_->GetDesc().NumDescriptors : 0));
                if(is_compute)
                    command_list_->SetComputeRootDescriptorTable(i, sampler_descriptors_.getGPUHandle(descriptor_slot));
                else
                    command_list_->SetGraphicsRootDescriptorTable(i, sampler_descriptors_.getGPUHandle(descriptor_slot));
                continue;   // done
            }
            uint32_t descriptor_slot = (parameter.descriptor_slot_ != 0xFFFFFFFFu ? parameter.descriptor_slot_ :
                                                                                    dummy_descriptors_[parameter.type_]);
            if(parameter.descriptor_slot_ != 0xFFFFFFFFu)
                initDescriptorParameter(kernel, program, invalidate_descriptors, parameter, descriptor_slot);
            GFX_ASSERT(descriptor_slot < (descriptors_.descriptor_heap_ != nullptr ? descriptors_.descriptor_heap_->GetDesc().NumDescriptors : 0));
            if(is_compute)
                command_list_->SetComputeRootDescriptorTable(i, descriptors_.getGPUHandle(descriptor_slot));
            else
                command_list_->SetGraphicsRootDescriptorTable(i, descriptors_.getGPUHandle(descriptor_slot));
        }
        if(sbt != nullptr)
        {
            ID3D12StateObjectProperties *state_object_properties = nullptr;
            kernel.state_object_->QueryInterface(IID_PPV_ARGS(&state_object_properties));
            bool const invalidate_sbt_descriptors = sbt->descriptor_heap_id_ != descriptor_heap_id;
            bool const invalidate_sbt_parameters = sbt->kernel_ != bound_kernel_;
            sbt->descriptor_heap_id_ = descriptor_heap_id; // update descriptor heap id
            sbt->kernel_ = bound_kernel_; // update bound kernel
            bool transitioned = false;
            for(uint32_t i = 0; i < kGfxShaderGroupType_Count; ++i)
            {
                Buffer &sbt_buffer = buffers_[sbt->sbt_buffers_[i]];
                bool    transition = false;
                for(auto &shader_record : sbt->shader_records_[i])
                {
                    Sbt::ShaderRecord &sbt_record = shader_record.second;
                    if(sbt_record.id_ == sbt_record.commited_id_ && !invalidate_sbt_descriptors && !invalidate_sbt_parameters) continue;
                    transition = true;
                }
                if(transition)
                    transitionResource(sbt_buffer, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit);
            }
            if(transitioned)
                submitPipelineBarriers();
            for(uint32_t i = 0; i < kGfxShaderGroupType_Count; ++i)
            {
                auto last_record_it = sbt->shader_records_[i].rbegin();
                if(last_record_it == sbt->shader_records_[i].rend()) continue;
                uint32_t record_count = last_record_it->first + 1;
                size_t sbt_buffer_size = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - 1 + kernel.sbt_record_stride_[i] * record_count;
                GfxBuffer const upload_gfx_buffer = createBuffer(sbt_buffer_size, nullptr, kGfxCpuAccess_Write);
                Buffer &upload_buffer = buffers_[upload_gfx_buffer];
                Buffer &sbt_buffer = buffers_[sbt->sbt_buffers_[i]];
                uint64_t upload_buffer_offset = upload_buffer.data_offset_;
                for(auto &shader_record : sbt->shader_records_[i])
                {
                    uint32_t sbt_index = shader_record.first;
                    Sbt::ShaderRecord &sbt_record = shader_record.second;
                    if(sbt_record.id_ == sbt_record.commited_id_ && !invalidate_sbt_descriptors && !invalidate_sbt_parameters) continue;
                    uint64_t dst_offset = GFX_ALIGN(sbt_buffer.resource_->GetGPUVirtualAddress(), D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT) -
                        sbt_buffer.resource_->GetGPUVirtualAddress() + sbt_index * kernel.sbt_record_stride_[i];
                    uint64_t const src_offset = upload_buffer_offset;
                    void *shader_identifier = state_object_properties->GetShaderIdentifier(sbt_record.shader_identifier_.c_str());
                    memcpy((uint8_t *)upload_buffer.data_ + upload_buffer_offset, shader_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
                    upload_buffer_offset += D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
                    auto local_root_signature_association = kernel.local_root_signature_associations_.find(sbt_record.shader_identifier_);
                    if(local_root_signature_association != kernel.local_root_signature_associations_.end())
                    {
                        for(auto &parameter : sbt_record.bound_parameters_)
                        {
                            if(parameter.type_ >= Kernel::Parameter::kType_Count) continue;
                            switch(parameter.type_)
                            {
                            case Kernel::Parameter::kType_Constants:
                            case Kernel::Parameter::kType_ConstantBuffer:
                                populateRootConstants(program, *sbt_record.parameters_, parameter,
                                    (uint32_t *)((uint8_t *)upload_buffer.data_ + upload_buffer_offset), true);
                                upload_buffer_offset += parameter.variable_size_;
                                break;
                            default:
                                uint32_t descriptor_slot = (parameter.descriptor_slot_ != 0xFFFFFFFFu
                                                                ? parameter.descriptor_slot_
                                                                : dummy_descriptors_[parameter.type_]);
                                if(parameter.descriptor_slot_ != 0xFFFFFFFFu)
                                    initDescriptorParameter(kernel, program, invalidate_sbt_descriptors, parameter, descriptor_slot);
                                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                                {
                                    auto descriptor_handle =
                                        descriptors_.getGPUHandle(descriptor_slot + j);
                                    memcpy((uint8_t *)upload_buffer.data_ + upload_buffer_offset,
                                        &descriptor_handle, sizeof(descriptor_handle));
                                    upload_buffer_offset += sizeof(descriptor_handle);
                                }
                                break;
                            }
                        }
                    }
                    command_list_->CopyBufferRegion(sbt_buffer.resource_, dst_offset, upload_buffer.resource_,
                        src_offset, upload_buffer_offset - src_offset);
                    sbt_record.commited_id_ = sbt_record.id_;
                }
                destroyBuffer(upload_gfx_buffer);
                transitionResource(sbt_buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            state_object_properties->Release();
        }
        if(kernel.isGraphics())
        {
            if(indexed && (force_install_index_buffer_ || bound_index_buffer_.handle != installed_index_buffer_.handle))
            {
                D3D12_INDEX_BUFFER_VIEW ibv_desc = {};
                if(!buffer_handles_.has_handle(bound_index_buffer_.handle))
                {
                    bound_index_buffer_ = {};
                    if(bound_index_buffer_.handle != 0)
                        GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid buffer object for use as index buffer");
                }
                else
                {
                    Buffer &gfx_buffer = buffers_[bound_index_buffer_];
                    SetObjectName(gfx_buffer, bound_index_buffer_.name);
                    ibv_desc.BufferLocation = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
                    ibv_desc.SizeInBytes = (uint32_t)bound_index_buffer_.size;
                    ibv_desc.Format = (bound_index_buffer_.stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT);
                    if(bound_index_buffer_.cpu_access == kGfxCpuAccess_None)
                        transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER, kTransitionType_Implicit);
                }
                command_list_->IASetIndexBuffer(&ibv_desc);
                installed_index_buffer_ = bound_index_buffer_;
                force_install_index_buffer_ = false;
            }
            if(kernel.vertex_stride_ > 0 && (force_install_vertex_buffer_ || bound_vertex_buffer_ != installed_vertex_buffer_))
            {
                D3D12_VERTEX_BUFFER_VIEW vbv_desc = {};
                if(!buffer_handles_.has_handle(bound_vertex_buffer_.handle))
                {
                    bound_vertex_buffer_ = {};
                    if(bound_vertex_buffer_.handle != 0)
                        GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid buffer object for use as vertex buffer");
                }
                else
                {
                    Buffer &gfx_buffer = buffers_[bound_vertex_buffer_];
                    SetObjectName(gfx_buffer, bound_vertex_buffer_.name);
                    vbv_desc.BufferLocation = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
                    vbv_desc.SizeInBytes = (uint32_t)bound_vertex_buffer_.size;
                    vbv_desc.StrideInBytes = GFX_MAX(bound_vertex_buffer_.stride, kernel.vertex_stride_);
                    if(bound_vertex_buffer_.cpu_access == kGfxCpuAccess_None)
                        transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, kTransitionType_Implicit);
                }
                command_list_->IASetVertexBuffers(0, 1, &vbv_desc);
                installed_vertex_buffer_ = bound_vertex_buffer_;
                force_install_vertex_buffer_ = false;
            }
        }
        return kGfxResult_NoError;
    }

    GfxResult ensureTextureHasUsageFlag(Texture &texture, D3D12_RESOURCE_FLAGS usage_flag)
    {
        D3D12_RESOURCE_DESC resource_desc = texture.resource_->GetDesc();
        if(!((resource_desc.Flags & usage_flag) != 0))
        {
            if(texture.isInterop())
                return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot re-create interop texture objects with different usage flag(s)");
            ID3D12Resource *resource = nullptr;
            D3D12MA::Allocation *allocation = nullptr;
            D3D12MA::ALLOCATION_DESC allocation_desc = {};
            allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            resource_desc.Alignment = 0;    // default alignment
            resource_desc.Flags |= usage_flag;  // add usage flag
            GFX_TRY(createResource(allocation_desc, resource_desc, D3D12_RESOURCE_STATE_COMMON, texture.clear_value_, &allocation, IID_PPV_ARGS(&resource)));
            bool transition = transitionResource(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, kTransitionType_Implicit);
            ID3D12Resource *previous_resource = texture.resource_;
            collect(texture);   // release previous texture
            texture.Object::flags_ &= ~Object::kFlag_Named;
            texture.allocation_ = allocation;
            texture.resource_ = resource;
            texture.resource_state_ = D3D12_RESOURCE_STATE_COMMON;
            texture.initial_resource_state_ = D3D12_RESOURCE_STATE_COMMON;
            texture.transitioned_ = false;
            transition |= transitionResource(texture, D3D12_RESOURCE_STATE_COPY_DEST, kTransitionType_Implicit);
            if(transition) submitPipelineBarriers();
            command_list_->CopyResource(resource, previous_resource);
            for(uint32_t i = 0; i < ARRAYSIZE(texture.dsv_descriptor_slots_); ++i)
            {
                texture.dsv_descriptor_slots_[i].resize(resource_desc.DepthOrArraySize);
                for(size_t j = 0; j < texture.dsv_descriptor_slots_[i].size(); ++j)
                    texture.dsv_descriptor_slots_[i][j] = 0xFFFFFFFFu;
            }
            for(uint32_t i = 0; i < ARRAYSIZE(texture.rtv_descriptor_slots_); ++i)
            {
                texture.rtv_descriptor_slots_[i].resize(resource_desc.DepthOrArraySize);
                for(size_t j = 0; j < texture.rtv_descriptor_slots_[i].size(); ++j)
                    texture.rtv_descriptor_slots_[i][j] = 0xFFFFFFFFu;
            }
        }
        return kGfxResult_NoError;
    }

    GfxResult ensureTextureHasDepthStencilView(GfxTexture const &texture, Texture &gfx_texture, uint32_t mip_level, uint32_t slice)
    {
        GFX_TRY(ensureTextureHasUsageFlag(gfx_texture, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
        GFX_ASSERT(mip_level < ARRAYSIZE(gfx_texture.dsv_descriptor_slots_));
        GFX_ASSERT(slice < gfx_texture.dsv_descriptor_slots_[mip_level].size());
        if(gfx_texture.dsv_descriptor_slots_[mip_level][slice] == 0xFFFFFFFFu)
        {
            gfx_texture.dsv_descriptor_slots_[mip_level][slice] = allocateDSVDescriptor();
            if(gfx_texture.dsv_descriptor_slots_[mip_level][slice] == 0xFFFFFFFFu)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to allocate DSV descriptor");
            D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
            D3D12_DEPTH_STENCIL_VIEW_DESC
            dsv_desc        = {};
            dsv_desc.Format = resource_desc.Format;
            if(texture.is2D())
            {
                dsv_desc.ViewDimension      = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsv_desc.Texture2D.MipSlice = mip_level;
            }
            else
            {
                dsv_desc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsv_desc.Texture2DArray.MipSlice        = mip_level;
                dsv_desc.Texture2DArray.FirstArraySlice = slice;
                dsv_desc.Texture2DArray.ArraySize       = 1;
            }
            device_->CreateDepthStencilView(gfx_texture.resource_, &dsv_desc, dsv_descriptors_.getCPUHandle(gfx_texture.dsv_descriptor_slots_[mip_level][slice]));
        }
        return kGfxResult_NoError;
    }

    GfxResult ensureTextureHasRenderTargetView(GfxTexture const &texture, Texture &gfx_texture, uint32_t mip_level, uint32_t slice)
    {
        GFX_TRY(ensureTextureHasUsageFlag(gfx_texture, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
        GFX_ASSERT(mip_level < ARRAYSIZE(gfx_texture.rtv_descriptor_slots_));
        GFX_ASSERT(slice < gfx_texture.rtv_descriptor_slots_[mip_level].size());
        if(gfx_texture.rtv_descriptor_slots_[mip_level][slice] == 0xFFFFFFFFu)
        {
            gfx_texture.rtv_descriptor_slots_[mip_level][slice] = allocateRTVDescriptor();
            if(gfx_texture.rtv_descriptor_slots_[mip_level][slice] == 0xFFFFFFFFu)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to allocate RTV descriptor");
            D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
            D3D12_RENDER_TARGET_VIEW_DESC
            rtv_desc        = {};
            rtv_desc.Format = resource_desc.Format;
            if(texture.is2D())
            {
                rtv_desc.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtv_desc.Texture2D.MipSlice = mip_level;
            }
            else if(texture.is3D())
            {
                rtv_desc.ViewDimension         = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtv_desc.Texture3D.MipSlice    = mip_level;
                rtv_desc.Texture3D.FirstWSlice = slice;
                rtv_desc.Texture3D.WSize       = 1;
            }
            else
            {
                rtv_desc.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv_desc.Texture2DArray.MipSlice        = mip_level;
                rtv_desc.Texture2DArray.FirstArraySlice = slice;
                rtv_desc.Texture2DArray.ArraySize       = 1;
            }
            device_->CreateRenderTargetView(gfx_texture.resource_, &rtv_desc, rtv_descriptors_.getCPUHandle(gfx_texture.rtv_descriptor_slots_[mip_level][slice]));
        }
        return kGfxResult_NoError;
    }

    GfxBuffer allocateConstantMemory(uint64_t data_size)
    {
        GfxBuffer buffer = {};
        Buffer *constant_buffer = buffers_.at(constant_buffer_pool_[fence_index_]);
        uint64_t constant_buffer_size = (constant_buffer != nullptr ? constant_buffer->resource_->GetDesc().Width : 0);
        if(constant_buffer_pool_cursors_[fence_index_] * 256 + data_size > constant_buffer_size || constant_buffer == nullptr)
        {
            constant_buffer_size += data_size;
            constant_buffer_size += ((constant_buffer_size + 2) >> 1);
            constant_buffer_size = GFX_ALIGN(constant_buffer_size, 65536);
            destroyBuffer(constant_buffer_pool_[fence_index_]); // release previous memory
            constant_buffer_pool_[fence_index_] = createBuffer(constant_buffer_size, nullptr, kGfxCpuAccess_Write);
            if(!constant_buffer_pool_[fence_index_]) { return buffer; } // out of memory
            GFX_SNPRINTF(constant_buffer_pool_[fence_index_].name, sizeof(constant_buffer_pool_[fence_index_].name), "gfx_ConstantBufferPool%u", fence_index_);
            constant_buffer = &buffers_[constant_buffer_pool_[fence_index_]];
            SetObjectName(*constant_buffer, constant_buffer_pool_[fence_index_].name);
        }
        GFX_ASSERT(constant_buffer != nullptr && constant_buffer->data_ != nullptr);
        uint64_t const data_offset = constant_buffer_pool_cursors_[fence_index_] * 256;
        buffer = createBufferRange(constant_buffer_pool_[fence_index_], data_offset, data_size);
        constant_buffer_pool_cursors_[fence_index_] += (data_size + 255) / 256;
        return buffer;
    }

    D3D12_GPU_VIRTUAL_ADDRESS allocateConstantMemory(uint64_t data_size, void *&data)
    {
        D3D12_GPU_VIRTUAL_ADDRESS gpu_addr = {};
        Buffer *constant_buffer = buffers_.at(constant_buffer_pool_[fence_index_]);
        uint64_t constant_buffer_size = (constant_buffer != nullptr ? constant_buffer->resource_->GetDesc().Width : 0);
        if(constant_buffer_pool_cursors_[fence_index_] * 256 + data_size > constant_buffer_size || constant_buffer == nullptr)
        {
            constant_buffer_size += data_size;
            constant_buffer_size += ((constant_buffer_size + 2) >> 1);
            constant_buffer_size = GFX_ALIGN(constant_buffer_size, 65536);
            destroyBuffer(constant_buffer_pool_[fence_index_]); // release previous memory
            constant_buffer_pool_[fence_index_] = createBuffer(constant_buffer_size, nullptr, kGfxCpuAccess_Write);
            if(!constant_buffer_pool_[fence_index_]) { data = nullptr; return gpu_addr; }   // out of memory
            GFX_SNPRINTF(constant_buffer_pool_[fence_index_].name, sizeof(constant_buffer_pool_[fence_index_].name), "gfx_ConstantBufferPool%u", fence_index_);
            constant_buffer = &buffers_[constant_buffer_pool_[fence_index_]];
            SetObjectName(*constant_buffer, constant_buffer_pool_[fence_index_].name);
        }
        GFX_ASSERT(constant_buffer != nullptr && constant_buffer->data_ != nullptr);
        uint64_t const data_offset = constant_buffer_pool_cursors_[fence_index_] * 256;
        gpu_addr = constant_buffer->resource_->GetGPUVirtualAddress() + data_offset;
        constant_buffer_pool_cursors_[fence_index_] += (data_size + 255) / 256;
        data = (char *)constant_buffer->data_ + data_offset;
        return gpu_addr;
    }

    GfxResult allocateRaytracingScratch(uint64_t scratch_data_size)
    {
        if(scratch_data_size > raytracing_scratch_buffer_.size)
        {
            destroyBuffer(raytracing_scratch_buffer_);
            scratch_data_size = GFX_ALIGN(scratch_data_size, 65536);
            raytracing_scratch_buffer_ = createBuffer(scratch_data_size, nullptr, kGfxCpuAccess_None);
            if(!raytracing_scratch_buffer_)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to create raytracing scratch buffer");
            strcpy(raytracing_scratch_buffer_.name, "gfx_RaytracingScratchBuffer");
            Buffer &gfx_buffer = buffers_[raytracing_scratch_buffer_];
            SetObjectName(gfx_buffer, raytracing_scratch_buffer_.name);
        }
        GFX_ASSERT(buffer_handles_.has_handle(raytracing_scratch_buffer_.handle));
        return kGfxResult_NoError;
    }

    GfxResult buildRaytracingPrimitive(GfxRaytracingPrimitive const &raytracing_primitive, RaytracingPrimitive &gfx_raytracing_primitive, bool update)
    {
        switch(gfx_raytracing_primitive.type_)
        {
        case RaytracingPrimitive::kType_Triangles:
            GFX_TRY(buildRaytracingPrimitiveTriangles(raytracing_primitive, gfx_raytracing_primitive, update));
            break;
        case RaytracingPrimitive::kType_Instance:
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot build an instance raytracing primitive");
        case RaytracingPrimitive::kType_Procedural:
            GFX_TRY(buildRaytracingPrimitiveProcedural(raytracing_primitive, gfx_raytracing_primitive, update));
            break;
        default:
            GFX_ASSERTMSG(0, "An invalid raytracing primitive type was supplied");
            break;
        }
        return kGfxResult_NoError;
    }

    GfxResult buildRaytracingPrimitiveTriangles(GfxRaytracingPrimitive const &raytracing_primitive, RaytracingPrimitive &gfx_raytracing_primitive, bool update)
    {
        GFX_ASSERT(gfx_raytracing_primitive.type_ == RaytracingPrimitive::kType_Triangles); // should never happen
        if(gfx_raytracing_primitive.triangles_.index_stride_ != 0 && !buffer_handles_.has_handle(gfx_raytracing_primitive.triangles_.index_buffer_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive that's pointing to an invalid index buffer object");
        if(!buffer_handles_.has_handle(gfx_raytracing_primitive.triangles_.vertex_buffer_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive that's pointing to an invalid vertex buffer object");
        GFX_ASSERT(gfx_raytracing_primitive.triangles_.index_stride_ == 0 || gfx_raytracing_primitive.triangles_.index_buffer_.size / gfx_raytracing_primitive.triangles_.index_stride_ <= 0xFFFFFFFFull);
        GFX_ASSERT(gfx_raytracing_primitive.triangles_.vertex_stride_ > 0 && gfx_raytracing_primitive.triangles_.vertex_buffer_.size / gfx_raytracing_primitive.triangles_.vertex_stride_ <= 0xFFFFFFFFull);
        Buffer *gfx_index_buffer = (gfx_raytracing_primitive.triangles_.index_stride_ != 0 ? &buffers_[gfx_raytracing_primitive.triangles_.index_buffer_] : nullptr);
        if(gfx_index_buffer != nullptr) SetObjectName(*gfx_index_buffer, gfx_raytracing_primitive.triangles_.index_buffer_.name);
        Buffer &gfx_vertex_buffer = buffers_[gfx_raytracing_primitive.triangles_.vertex_buffer_];
        SetObjectName(gfx_vertex_buffer, gfx_raytracing_primitive.triangles_.vertex_buffer_.name);
        GFX_TRY(updateRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive));
        if((gfx_raytracing_primitive.triangles_.index_stride_ != 0 && gfx_raytracing_primitive.triangles_.index_buffer_.size == 0) ||
            gfx_raytracing_primitive.triangles_.vertex_buffer_.size == 0)
        {
            destroyBuffer(gfx_raytracing_primitive.triangles_.bvh_buffer_);
            gfx_raytracing_primitive.triangles_.bvh_buffer_ = {};
            return kGfxResult_NoError;
        }
        D3D12_RAYTRACING_GEOMETRY_DESC geometry_desc = {};
        geometry_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        if((gfx_raytracing_primitive.triangles_.build_flags_ & kGfxBuildRaytracingPrimitiveFlag_Opaque) != 0)
            geometry_desc.Flags |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        GFX_ASSERT(gfx_raytracing_primitive.triangles_.index_stride_ == 0 || gfx_index_buffer != nullptr);  // should never happen
        if(gfx_index_buffer != nullptr)
        {
            geometry_desc.Triangles.IndexFormat = (gfx_raytracing_primitive.triangles_.index_stride_ == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT);
            geometry_desc.Triangles.IndexCount = (uint32_t)(gfx_raytracing_primitive.triangles_.index_buffer_.size / gfx_raytracing_primitive.triangles_.index_stride_);
            geometry_desc.Triangles.IndexBuffer = gfx_index_buffer->resource_->GetGPUVirtualAddress() + gfx_index_buffer->data_offset_;
        }
        geometry_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry_desc.Triangles.VertexCount = (uint32_t)(gfx_raytracing_primitive.triangles_.vertex_buffer_.size / gfx_raytracing_primitive.triangles_.vertex_stride_);
        geometry_desc.Triangles.VertexBuffer.StartAddress = gfx_vertex_buffer.resource_->GetGPUVirtualAddress() + gfx_vertex_buffer.data_offset_;
        geometry_desc.Triangles.VertexBuffer.StrideInBytes = gfx_raytracing_primitive.triangles_.vertex_stride_;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs = {};
        blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        blas_inputs.NumDescs = 1;
        blas_inputs.pGeometryDescs = &geometry_desc;
        if(update)
            blas_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info = {};
        dxr_device_->GetRaytracingAccelerationStructurePrebuildInfo(&blas_inputs, &blas_info);
        uint64_t const scratch_data_size = GFX_MAX(blas_info.ScratchDataSizeInBytes, blas_info.UpdateScratchDataSizeInBytes);
        GFX_TRY(allocateRaytracingScratch(scratch_data_size));  // ensure scratch is large enough
        uint64_t const bvh_data_size = GFX_ALIGN(blas_info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        if(bvh_data_size > gfx_raytracing_primitive.triangles_.bvh_buffer_.size)
        {
            if(!gfx_raytracing_primitive.triangles_.bvh_buffer_)
            {
                GfxAccelerationStructure const &acceleration_structure = gfx_raytracing_primitive.triangles_.acceleration_structure_;
                GFX_ASSERT(acceleration_structure_handles_.has_handle(acceleration_structure.handle));  // checked in `updateRaytracingPrimitive()'
                AccelerationStructure &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
                gfx_acceleration_structure.needs_rebuild_ = true;   // raytracing primitive has been built, rebuild the acceleration structure
            }
            destroyBuffer(gfx_raytracing_primitive.triangles_.bvh_buffer_);
            blas_inputs.Flags &= ~D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            gfx_raytracing_primitive.triangles_.bvh_buffer_ = createBuffer(bvh_data_size, nullptr, kGfxCpuAccess_None, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            if(!gfx_raytracing_primitive.triangles_.bvh_buffer_)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to create raytracing primitive buffer");
        }
        gfx_raytracing_primitive.triangles_.bvh_data_size_ = (uint64_t)blas_info.ResultDataMaxSizeInBytes;
        GFX_ASSERT(buffer_handles_.has_handle(gfx_raytracing_primitive.triangles_.bvh_buffer_.handle));
        GFX_ASSERT(buffer_handles_.has_handle(raytracing_scratch_buffer_.handle));
        Buffer &gfx_buffer = buffers_[gfx_raytracing_primitive.triangles_.bvh_buffer_];
        Buffer &gfx_scratch_buffer = buffers_[raytracing_scratch_buffer_];
        SetObjectName(gfx_buffer, raytracing_primitive.name);
        bool transition = transitionResource(buffers_[gfx_raytracing_primitive.triangles_.vertex_buffer_], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kTransitionType_Implicit);
        if(gfx_raytracing_primitive.triangles_.index_stride_ != 0)
            transition |= transitionResource(buffers_[gfx_raytracing_primitive.triangles_.index_buffer_], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kTransitionType_Implicit);
        transition |= transitionResource(gfx_scratch_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(transition)
            submitPipelineBarriers();   // ensure scratch is not in use
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc = {};
        build_desc.DestAccelerationStructureData = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
        build_desc.Inputs = blas_inputs;
        if((blas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0)
            build_desc.SourceAccelerationStructureData = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
        build_desc.ScratchAccelerationStructureData = gfx_scratch_buffer.resource_->GetGPUVirtualAddress() + gfx_scratch_buffer.data_offset_;
        GFX_ASSERT(dxr_command_list_ != nullptr);   // should never happen
        dxr_command_list_->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);
        return kGfxResult_NoError;
    }

    GfxResult buildRaytracingPrimitiveProcedural(GfxRaytracingPrimitive const &raytracing_primitive, RaytracingPrimitive &gfx_raytracing_primitive, bool update)
    {
        GFX_ASSERT(gfx_raytracing_primitive.type_ == RaytracingPrimitive::kType_Procedural); // should never happen
        if(!buffer_handles_.has_handle(gfx_raytracing_primitive.procedural_.procedural_buffer_.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive that's pointing to an invalid procedural buffer object");
        GFX_ASSERT(gfx_raytracing_primitive.procedural_.procedural_stride_ > 0 && gfx_raytracing_primitive.procedural_.procedural_buffer_.size / gfx_raytracing_primitive.procedural_.procedural_stride_ <= 0xFFFFFFFFull);
        GFX_TRY(updateRaytracingPrimitive(raytracing_primitive, gfx_raytracing_primitive));
        if(gfx_raytracing_primitive.procedural_.procedural_buffer_.size == 0)
        {
            destroyBuffer(gfx_raytracing_primitive.procedural_.bvh_buffer_);
            gfx_raytracing_primitive.procedural_.bvh_buffer_ = {};
            return kGfxResult_NoError;
        }
        D3D12_RAYTRACING_GEOMETRY_DESC geometry_desc = {};
        geometry_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        if((gfx_raytracing_primitive.procedural_.build_flags_ & kGfxBuildRaytracingPrimitiveFlag_Opaque) != 0)
            geometry_desc.Flags |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        Buffer &gfx_aabb_buffer = buffers_[gfx_raytracing_primitive.procedural_.procedural_buffer_];
        SetObjectName(gfx_aabb_buffer, gfx_raytracing_primitive.procedural_.procedural_buffer_.name);
        geometry_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometry_desc.AABBs.AABBCount = 1;
        geometry_desc.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
        geometry_desc.AABBs.AABBs.StartAddress = gfx_aabb_buffer.resource_->GetGPUVirtualAddress() + gfx_aabb_buffer.data_offset_;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs = {};
        blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        blas_inputs.NumDescs = 1;
        blas_inputs.pGeometryDescs = &geometry_desc;
        if(update)
            blas_inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info = {};
        dxr_device_->GetRaytracingAccelerationStructurePrebuildInfo(&blas_inputs, &blas_info);
        uint64_t const scratch_data_size = GFX_MAX(blas_info.ScratchDataSizeInBytes, blas_info.UpdateScratchDataSizeInBytes);
        GFX_TRY(allocateRaytracingScratch(scratch_data_size));  // ensure scratch is large enough
        uint64_t const bvh_data_size = GFX_ALIGN(blas_info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        if(bvh_data_size > gfx_raytracing_primitive.procedural_.bvh_buffer_.size)
        {
            if(!gfx_raytracing_primitive.procedural_.bvh_buffer_)
            {
                GfxAccelerationStructure const &acceleration_structure = gfx_raytracing_primitive.procedural_.acceleration_structure_;
                GFX_ASSERT(acceleration_structure_handles_.has_handle(acceleration_structure.handle));  // checked in `updateRaytracingPrimitive()'
                AccelerationStructure &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
                gfx_acceleration_structure.needs_rebuild_ = true;   // raytracing primitive has been built, rebuild the acceleration structure
            }
            destroyBuffer(gfx_raytracing_primitive.procedural_.bvh_buffer_);
            blas_inputs.Flags &= ~D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            gfx_raytracing_primitive.procedural_.bvh_buffer_ = createBuffer(bvh_data_size, nullptr, kGfxCpuAccess_None, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            if(!gfx_raytracing_primitive.procedural_.bvh_buffer_)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to create raytracing primitive buffer");
        }
        gfx_raytracing_primitive.procedural_.bvh_data_size_ = (uint64_t)blas_info.ResultDataMaxSizeInBytes;
        GFX_ASSERT(buffer_handles_.has_handle(gfx_raytracing_primitive.procedural_.bvh_buffer_.handle));
        GFX_ASSERT(buffer_handles_.has_handle(raytracing_scratch_buffer_.handle));
        Buffer &gfx_buffer = buffers_[gfx_raytracing_primitive.procedural_.bvh_buffer_];
        Buffer &gfx_scratch_buffer = buffers_[raytracing_scratch_buffer_];
        SetObjectName(gfx_buffer, raytracing_primitive.name);
        bool transition = transitionResource(buffers_[gfx_raytracing_primitive.procedural_.procedural_buffer_], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kTransitionType_Implicit);
        transition |= transitionResource(gfx_scratch_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(transition)
            submitPipelineBarriers();   // ensure scratch is not in use
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc = {};
        build_desc.DestAccelerationStructureData = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
        build_desc.Inputs = blas_inputs;
        if((blas_inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0)
            build_desc.SourceAccelerationStructureData = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
        build_desc.ScratchAccelerationStructureData = gfx_scratch_buffer.resource_->GetGPUVirtualAddress() + gfx_scratch_buffer.data_offset_;
        GFX_ASSERT(dxr_command_list_ != nullptr);   // should never happen
        dxr_command_list_->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);
        return kGfxResult_NoError;
    }

    GfxResult updateRaytracingPrimitive(GfxRaytracingPrimitive const &raytracing_primitive, RaytracingPrimitive &gfx_raytracing_primitive)
    {
        GfxAccelerationStructure const &acceleration_structure = getRaytracingPrimitiveAccelerationStructure(gfx_raytracing_primitive);
        if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
            return GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot update a raytracing primitive that's pointing to an invalid acceleration structure object");
        AccelerationStructure &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
        if(gfx_raytracing_primitive.index_ >= (uint32_t)gfx_acceleration_structure.raytracing_primitives_.size() ||
           raytracing_primitive.handle != gfx_acceleration_structure.raytracing_primitives_[gfx_raytracing_primitive.index_].handle)
        {
            uint32_t const raytracing_primitive_count = (uint32_t)gfx_acceleration_structure.raytracing_primitives_.size();
            for(gfx_raytracing_primitive.index_ = 0; gfx_raytracing_primitive.index_ < raytracing_primitive_count; ++gfx_raytracing_primitive.index_)
                if(gfx_acceleration_structure.raytracing_primitives_[gfx_raytracing_primitive.index_].handle == raytracing_primitive.handle) break;
            if(gfx_raytracing_primitive.index_ >= raytracing_primitive_count)
                return GFX_SET_ERROR(kGfxResult_InternalError, "Cannot update a raytracing primitive that does not belong to the acceleration structure object it was created from");
        }
        gfx_acceleration_structure.raytracing_primitives_[gfx_raytracing_primitive.index_] = raytracing_primitive;
        gfx_acceleration_structure.needs_update_ = true;
        return kGfxResult_NoError;
    }

    GfxBuffer const &getRaytracingPrimitiveBuffer(RaytracingPrimitive const &raytracing_primitive)
    {
        static GfxBuffer const invalid_buffer = {};
        switch(raytracing_primitive.type_)
        {
        case RaytracingPrimitive::kType_Triangles:
            return raytracing_primitive.triangles_.bvh_buffer_;
        case RaytracingPrimitive::kType_Instance:
            {
                if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.instance_.parent_.handle))
                    return invalid_buffer;  // cannot get buffer from an invalid raytracing primitive
                RaytracingPrimitive const &parent_raytracing_primitive = raytracing_primitives_[raytracing_primitive.instance_.parent_];
                GFX_ASSERT(parent_raytracing_primitive.type_ != RaytracingPrimitive::kType_Instance);  // should never happen
                return getRaytracingPrimitiveBuffer(parent_raytracing_primitive);
            }
        case RaytracingPrimitive::kType_Procedural:
            return raytracing_primitive.procedural_.bvh_buffer_;
        default:
            GFX_ASSERTMSG(0, "An invalid raytracing primitive type was supplied");
            break;  // invalid raytracing primitive type
        }
        return invalid_buffer;
    }

    GfxAccelerationStructure const &getRaytracingPrimitiveAccelerationStructure(RaytracingPrimitive const &raytracing_primitive)
    {
        static GfxAccelerationStructure const invalid_acceleration_structure = {};
        switch(raytracing_primitive.type_)
        {
        case RaytracingPrimitive::kType_Triangles:
            return raytracing_primitive.triangles_.acceleration_structure_;
        case RaytracingPrimitive::kType_Instance:
            {
                if(!raytracing_primitive_handles_.has_handle(raytracing_primitive.instance_.parent_.handle))
                    return invalid_acceleration_structure;  // cannot get acceleration structure from an invalid raytracing primitive
                RaytracingPrimitive const &parent_raytracing_primitive = raytracing_primitives_[raytracing_primitive.instance_.parent_];
                GFX_ASSERT(parent_raytracing_primitive.type_ != RaytracingPrimitive::kType_Instance);  // should never happen
                return getRaytracingPrimitiveAccelerationStructure(parent_raytracing_primitive);
            }
        case RaytracingPrimitive::kType_Procedural:
            return raytracing_primitive.procedural_.acceleration_structure_;
        default:
            GFX_ASSERTMSG(0, "An invalid raytracing primitive type was supplied");
            break;  // invalid raytracing primitive type
        }
        return invalid_acceleration_structure;
    }

    MipKernels const &getMipKernels(GfxTexture const &texture)
    {
        auto const texture_type = texture.type;
        uint32_t const channels = GetChannelCount(texture.format);
        uint32_t const key = ((texture_type << 2) | channels);  // lookup key
        std::map<uint32_t, MipKernels>::const_iterator const it = mip_kernels_.find(key);
        if(it != mip_kernels_.end()) return (*it).second;   // already compiled
        char const *texture_type_str = nullptr, *channel_type_str = nullptr, *did_type_str = nullptr, *select_string = nullptr;
        switch(texture_type)
        {
        case GfxTexture::kType_2D:
            texture_type_str = "RWTexture2D";
            did_type_str = "uint2";
            break;
        case GfxTexture::kType_2DArray:
        case GfxTexture::kType_Cube:
            texture_type_str = "RWTexture2DArray";
            did_type_str = "uint3";
            break;
        case GfxTexture::kType_3D:
            texture_type_str = "RWTexture3D";
            did_type_str = "uint3";
            break;
        default:
            GFX_ASSERT(0);
            break;  // should never get here
        }
        switch(channels)
        {
        case 1:
            channel_type_str = "float";
            select_string = "float select(bool a, float b, float c){return a ? b : c;}";
            break;
        case 2:
            channel_type_str = "float2";
            select_string = "float2 select(bool2 a, float2 b, float2 c){return a ? b : c;}";
            break;
        case 3:
            channel_type_str = "float3";
            select_string = "float3 select(bool3 a, float3 b, float3 c){return a ? b : c;}";
            break;
        case 4:
            channel_type_str = "float4";
            select_string = "float3 select(bool3 a, float3 b, float3 c){return a ? b : c;}";
            break;
        default:
            GFX_ASSERT(0);
            break;  // should never get here
        }
        std::string texture_type_combined = texture_type_str;
        texture_type_combined += '<';
        texture_type_combined += channel_type_str;
        texture_type_combined += '>';
        std::string mip_program_source = "#if __HLSL_VERSION < 2021\r\n";
        mip_program_source += select_string;
        mip_program_source += "\r\n#endif\r\n";
        mip_program_source += texture_type_combined;
        mip_program_source += " InputBuffer;\r\n";
        mip_program_source += texture_type_combined;
        mip_program_source += " OutputBuffer;\r\n"
            "bool isSRGB;\r\n"
            "\r\n";
        mip_program_source += channel_type_str;
        mip_program_source += " convertToSRGB(";
        mip_program_source += channel_type_str;
        mip_program_source += " val)\r\n"
            "{\r\n"
            "    if(isSRGB)\r\n"
            "        return ";
        mip_program_source += channel_type_str;
        std::string val_string = "val";
        if(channels == 4)
            val_string += ".xyz";
        mip_program_source += "(select(";
        mip_program_source += val_string;
        mip_program_source += " < 0.0031308f, 12.92f * ";
        mip_program_source += val_string;
        mip_program_source += ", 1.055f * pow(abs(";
        mip_program_source += val_string;
        mip_program_source += "), 1.0f / 2.4f) - 0.055f)";
        if(channels == 4)
            mip_program_source += ", val.w";
        mip_program_source += "); \r\n"
            "    else\r\n"
            "        return val;\r\n"
            "}\r\n";
        mip_program_source += channel_type_str;
        mip_program_source += " convertToLinear(";
        mip_program_source += channel_type_str;
        mip_program_source += " val)\r\n"
            "{\r\n"
            "    if(isSRGB)\r\n"
            "        return ";
        mip_program_source += channel_type_str;
        mip_program_source += "(select(";
        mip_program_source += val_string;
        mip_program_source += " < 0.04045f, ";
        mip_program_source += val_string;
        mip_program_source += " / 12.92f, pow((";
        mip_program_source += val_string;
        mip_program_source += " + 0.055f) / 1.055f, 2.4f))";
        if(channels == 4)
            mip_program_source += ", val.w";
        mip_program_source += "); \r\n"
            "    else\r\n"
            "        return val;\r\n"
            "}\r\n"
            "\r\n"
            "[numthreads(16, 16, 1)]\r\n"
            "void main(in ";
        mip_program_source += did_type_str;
        mip_program_source += " did : SV_DispatchThreadID)\r\n"
            "{\r\n"
            "    ";
        mip_program_source += did_type_str;
        mip_program_source += " dims;\r\n"
            "    float w = 0.0f;\r\n"
            "    InputBuffer.GetDimensions(dims.x, dims.y";
        if(texture_type != GfxTexture::kType_2D)
            mip_program_source += ", dims.z";
        mip_program_source += ");\r\n"
            "    if(any(did.xy >= max(dims.xy >> 1, 1))) return;\r\n"
            "    ";
        mip_program_source += channel_type_str;
        mip_program_source += " result = ";
        mip_program_source += "0.0f";
        if(channels == 2)
            mip_program_source += ".xx";
        if(channels == 3)
            mip_program_source += ".xxx";
        if(channels == 4)
            mip_program_source += ".xxxx";
        mip_program_source += "; \r\n"
            "    for(uint y = 0; y < 2; ++y)\r\n"
            "        for(uint x = 0; x < 2; ++x)\r\n"
            "        {\r\n"
            "            const uint2 pix = (did.xy << 1) + uint2(x, y);\r\n"
            "            if(any(pix >= dims.xy)) break; // out of bounds\r\n"
            "            result += convertToLinear(InputBuffer[";
        if(texture_type != GfxTexture::kType_2D)
            mip_program_source += "uint3(pix, did.z)";
        else
            mip_program_source += "pix";
        mip_program_source += "]);\r\n"
            "            ++w;\r\n"
            "        }\r\n"
            "    OutputBuffer[did] = convertToSRGB(result / w);\r\n"
            "}\r\n";

        MipKernels& mip_kernels = mip_kernels_[key];
        GfxProgramDesc mip_program_desc = {};
        mip_program_desc.cs = mip_program_source.c_str();
        mip_kernels.mip_program_ = createProgram(mip_program_desc, "gfx_GenerateMipsProgram", nullptr, nullptr, 0);
        mip_kernels.mip_kernel_ = createComputeKernel(mip_kernels.mip_program_, "main", nullptr, 0);
        return mip_kernels;
    }

    ScanKernels const &getScanKernels(OpType op_type, GfxDataType data_type, GfxBuffer const *count)
    {
        GFX_ASSERT(op_type < kOpType_Count);    // should never happen
        GFX_ASSERT(count == nullptr || buffer_handles_.has_handle(count->handle));
        uint32_t const key = (data_type << 3) | (op_type << 1) | (count != nullptr ? 1 : 0);
        std::map<uint32_t, ScanKernels>::const_iterator const it = scan_kernels_.find(key);
        if(it != scan_kernels_.end()) return (*it).second;  // already compiled
        char const *data_type_str = nullptr, *identity_str = nullptr;
        switch(data_type)
        {
        case kGfxDataType_Int:
            data_type_str = "int";
            identity_str = (op_type == kOpType_Min ? "2147483647" : op_type == kOpType_Max ? "-2147483648" : "0");
            break;
        case kGfxDataType_Uint:
            data_type_str = "uint";
            identity_str = (op_type == kOpType_Min ? "4294967295u" : "0");
            break;
        default:
            data_type_str = "float";
            identity_str = (op_type == kOpType_Min ? "3.402823466e+38f" : op_type == kOpType_Max ? "-3.402823466e+38f" : "0.0f");
            break;
        }
        std::string scan_program_source;
        scan_program_source +=
            "#define GROUP_SIZE      256\r\n"
            "#define KEYS_PER_THREAD 4\r\n"
            "#define KEYS_PER_GROUP  (GROUP_SIZE * KEYS_PER_THREAD)\r\n"
            "\r\n";
        if(count != nullptr)
            scan_program_source += "RWStructuredBuffer<uint> g_CountBuffer;\r\n";
        else
            scan_program_source += "uint g_Count;\r\n\r\n";
        scan_program_source +=
            "RWStructuredBuffer<";
        scan_program_source += data_type_str;
        scan_program_source += "> g_InputKeys;\r\n";
        scan_program_source +=
            "RWStructuredBuffer<";
        scan_program_source += data_type_str;
        scan_program_source += "> g_OutputKeys;\r\n";
        scan_program_source +=
            "RWStructuredBuffer<";
        scan_program_source += data_type_str;
        scan_program_source += "> g_PartialResults;\r\n";
        if(count != nullptr)
            scan_program_source +=
                "\r\n"
                "RWStructuredBuffer<uint4> g_Args1Buffer;\r\n"
                "RWStructuredBuffer<uint4> g_Args2Buffer;\r\n"
                "RWStructuredBuffer<uint>  g_Count1Buffer;\r\n"
                "RWStructuredBuffer<uint>  g_Count2Buffer;\r\n";
        scan_program_source +=
            "\r\n"
            "groupshared ";
        scan_program_source += data_type_str;
        scan_program_source += " lds_keys[GROUP_SIZE];\r\n";
        scan_program_source +=
            "groupshared ";
        scan_program_source += data_type_str;
        scan_program_source += " lds_loads[KEYS_PER_THREAD][GROUP_SIZE];\r\n";
        scan_program_source +=
            "\r\n"
            "uint GetCount()\r\n"
            "{\r\n";
        if(count != nullptr)
            scan_program_source += "    return g_CountBuffer[0];\r\n";
        else
            scan_program_source += "    return g_Count;\r\n";
        scan_program_source +=
            "}\r\n"
            "\r\n";
        scan_program_source += data_type_str;
        scan_program_source += " Op(in ";
        scan_program_source += data_type_str;
        scan_program_source += " lhs, in ";
        scan_program_source += data_type_str;
        scan_program_source += " rhs)\r\n";
        scan_program_source +=
            "{\r\n";
        switch(op_type)
        {
        case kOpType_Min:
            scan_program_source += "    return min(lhs, rhs);\r\n";
            break;
        case kOpType_Max:
            scan_program_source += "    return max(lhs, rhs);\r\n";
            break;
        default:
            scan_program_source += "    return lhs + rhs;\r\n";
            break;
        }
        scan_program_source +=
            "}\r\n"
            "\r\n";
        scan_program_source += data_type_str;
        scan_program_source += " GroupScan(in ";
        scan_program_source += data_type_str;
        scan_program_source += " key, in uint lidx)\r\n";
        scan_program_source +=
            "{\r\n"
            "    lds_keys[lidx] = key;\r\n"
            "    GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "    uint stride;\r\n"
            "    for(stride = 1; stride < GROUP_SIZE; stride <<= 1)\r\n"
            "    {\r\n"
            "        if(lidx < GROUP_SIZE / (2 * stride))\r\n"
            "        {\r\n"
            "            lds_keys[2 * (lidx + 1) * stride - 1] = Op(lds_keys[2 * (lidx + 1) * stride - 1],\r\n"
            "                                                       lds_keys[(2 * lidx + 1) * stride - 1]);\r\n"
            "        }\r\n"
            "        GroupMemoryBarrierWithGroupSync();\r\n"
            "    }\r\n"
            "\r\n"
            "    if(lidx == 0)\r\n"
            "        lds_keys[GROUP_SIZE - 1] = ";
        scan_program_source += identity_str;
        scan_program_source += ";\r\n";
        scan_program_source +=
            "    GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "    for(stride = (GROUP_SIZE >> 1); stride > 0; stride >>= 1)\r\n"
            "    {\r\n"
            "        if(lidx < GROUP_SIZE / (2 * stride))\r\n"
            "        {\r\n            ";
        scan_program_source += data_type_str;
        scan_program_source += " tmp = lds_keys[(2 * lidx + 1) * stride - 1];\r\n";
        scan_program_source +=
            "            lds_keys[(2 * lidx + 1) * stride - 1] = lds_keys[2 * (lidx + 1) * stride - 1];\r\n"
            "            lds_keys[2 * (lidx + 1) * stride - 1] = Op(lds_keys[2 * (lidx + 1) * stride - 1], tmp);\r\n"
            "        }\r\n"
            "        GroupMemoryBarrierWithGroupSync();\r\n"
            "    }\r\n"
            "\r\n"
            "    return lds_keys[lidx];\r\n"
            "}\r\n"
            "\r\n";
        scan_program_source += data_type_str;
        scan_program_source += " GroupReduce(in ";
        scan_program_source += data_type_str;
        scan_program_source += " key, in uint lidx)\r\n";
        scan_program_source +=
            "{\r\n"
            "    lds_keys[lidx] = key;\r\n"
            "    GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "    for(uint stride = (GROUP_SIZE >> 1); stride > 0; stride >>= 1)\r\n"
            "    {\r\n"
            "        if(lidx < stride)\r\n"
            "        {\r\n"
            "            lds_keys[lidx] = Op(lds_keys[lidx], lds_keys[lidx + stride]);\r\n"
            "        }\r\n"
            "        GroupMemoryBarrierWithGroupSync();\r\n"
            "    }\r\n"
            "\r\n"
            "    return lds_keys[0];\r\n"
            "}\r\n"
            "\r\n"
            "[numthreads(GROUP_SIZE, 1, 1)]\r\n"
            "void BlockScan(in uint gidx : SV_DispatchThreadID, in uint lidx : SV_GroupThreadID, in uint bidx : SV_GroupID)\r\n"
            "{\r\n"
            "    uint i, count = GetCount();\r\n"
            "\r\n"
            "    uint range_begin = bidx * GROUP_SIZE * KEYS_PER_THREAD;\r\n"
            "    for(i = 0; i < KEYS_PER_THREAD; ++i)\r\n"
            "    {\r\n"
            "        uint load_index = range_begin + i * GROUP_SIZE + lidx;\r\n"
            "\r\n"
            "        uint col = (i * GROUP_SIZE + lidx) / KEYS_PER_THREAD;\r\n"
            "        uint row = (i * GROUP_SIZE + lidx) % KEYS_PER_THREAD;\r\n"
            "\r\n"
            "        lds_loads[row][col] = (load_index < count ? g_InputKeys[load_index] : ";
        scan_program_source += identity_str;
        scan_program_source += ");\r\n";
        scan_program_source +=
            "    }\r\n"
            "    GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n    ";
        scan_program_source += data_type_str;
        scan_program_source += " thread_sum = ";
        scan_program_source += identity_str;
        scan_program_source += ";\r\n";
        scan_program_source +=
            "    for(i = 0; i < KEYS_PER_THREAD; ++i)\r\n"
            "    {\r\n        ";
        scan_program_source += data_type_str;
        scan_program_source += " tmp = lds_loads[i][lidx];\r\n";
        scan_program_source +=
            "        lds_loads[i][lidx] = thread_sum;\r\n"
            "        thread_sum = Op(thread_sum, tmp);\r\n"
            "    }\r\n"
            "    thread_sum = GroupScan(thread_sum, lidx);\r\n"
            "\r\n    ";
        scan_program_source += data_type_str;
        scan_program_source += " partial_result = ";
        scan_program_source += identity_str;
        scan_program_source += ";\r\n";
        scan_program_source +=
            "#ifdef PARTIAL_RESULT\r\n"
            "    partial_result = g_PartialResults[bidx];\r\n"
            "#endif\r\n"
            "\r\n"
            "    for(i = 0; i < KEYS_PER_THREAD; ++i)\r\n"
            "    {\r\n"
            "        lds_loads[i][lidx] = Op(lds_loads[i][lidx], thread_sum);\r\n"
            "    }\r\n"
            "    GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "    for(i = 0; i < KEYS_PER_THREAD; ++i)\r\n"
            "    {\r\n"
            "        uint store_index = range_begin + i * GROUP_SIZE + lidx;\r\n"
            "\r\n"
            "        uint col = (i * GROUP_SIZE + lidx) / KEYS_PER_THREAD;\r\n"
            "        uint row = (i * GROUP_SIZE + lidx) % KEYS_PER_THREAD;\r\n"
            "\r\n"
            "        if(store_index < count)\r\n"
            "        {\r\n"
            "            g_OutputKeys[store_index] = Op(lds_loads[row][col], partial_result);\r\n"
            "        }\r\n"
            "    }\r\n"
            "}\r\n"
            "\r\n"
            "[numthreads(GROUP_SIZE, 1, 1)]\r\n"
            "void BlockReduce(in uint gidx : SV_DispatchThreadID, in uint lidx : SV_GroupThreadID, in uint bidx : SV_GroupID)\r\n"
            "{\r\n    ";
        scan_program_source += data_type_str;
        scan_program_source += " thread_sum = ";
        scan_program_source += identity_str;
        scan_program_source += ";\r\n";
        scan_program_source +=
            "\r\n"
            "    uint range_begin = bidx * GROUP_SIZE * KEYS_PER_THREAD, count = GetCount();\r\n"
            "    for(uint i = 0; i < KEYS_PER_THREAD; ++i)\r\n"
            "    {\r\n"
            "        uint load_index = range_begin + i * GROUP_SIZE + lidx;\r\n"
            "        thread_sum = Op(load_index < count ? g_InputKeys[load_index] : ";
        scan_program_source += identity_str;
        scan_program_source += ", thread_sum);\r\n";
        scan_program_source +=
            "    }\r\n"
            "    thread_sum = GroupReduce(thread_sum, lidx);\r\n"
            "\r\n"
            "    if(lidx == 0)\r\n"
            "    {\r\n"
            "        g_PartialResults[bidx] = thread_sum;\r\n"
            "    }\r\n"
            "}\r\n";
        if(count != nullptr)
        {
            scan_program_source +=
                "\r\n"
                "[numthreads(1, 1, 1)]\r\n"
                "void GenerateDispatches()\r\n"
                "{\r\n"
                "    uint num_keys = g_CountBuffer[0];\r\n"
                "    uint num_groups_level_1 = (num_keys + KEYS_PER_GROUP - 1) / KEYS_PER_GROUP;\r\n"
                "    uint num_groups_level_2 = (num_groups_level_1 + KEYS_PER_GROUP - 1) / KEYS_PER_GROUP;\r\n"
                "    g_Args1Buffer[0] = uint4(num_groups_level_1, 1, 1, 0);\r\n"
                "    g_Args2Buffer[0] = uint4(num_groups_level_2, 1, 1, 0);\r\n"
                "    g_Count1Buffer[0] = num_groups_level_1;\r\n"
                "    g_Count2Buffer[0] = num_groups_level_2;\r\n"
                "}\r\n";
        }
        ScanKernels &scan_kernels = scan_kernels_[key];
        GfxProgramDesc scan_program_desc = {};
        scan_program_desc.cs = scan_program_source.c_str();
        char const *scan_add_defines[] = { "PARTIAL_RESULT" };
        scan_kernels.scan_program_ = createProgram(scan_program_desc, "gfx_ScanProgram", nullptr, nullptr, 0);
        scan_kernels.reduce_kernel_ = createComputeKernel(scan_kernels.scan_program_, "BlockReduce", nullptr, 0);
        scan_kernels.scan_add_kernel_ = createComputeKernel(scan_kernels.scan_program_, "BlockScan", scan_add_defines, ARRAYSIZE(scan_add_defines));
        scan_kernels.scan_kernel_ = createComputeKernel(scan_kernels.scan_program_, "BlockScan", nullptr, 0);
        if(count != nullptr)
            scan_kernels.args_kernel_ = createComputeKernel(scan_kernels.scan_program_, "GenerateDispatches", nullptr, 0);
        return scan_kernels;
    }

    SortKernels const &getSortKernels(bool sort_values, GfxBuffer const *count)
    {
        GFX_ASSERT(count == nullptr || buffer_handles_.has_handle(count->handle));
        uint32_t const key = ((sort_values ? 1 : 0) << 1) | (count != nullptr ? 1 : 0);
        std::map<uint32_t, SortKernels>::const_iterator const it = sort_kernels_.find(key);
        if(it != sort_kernels_.end()) return (*it).second;  // already compiled
        std::string sort_program_source;
        sort_program_source +=
            "#define NUM_BITS_PER_PASS   4\r\n"
            "#define NUM_BINS            (1 << NUM_BITS_PER_PASS)\r\n"
            "#define GROUP_SIZE          256\r\n"
            "#define KEYS_PER_THREAD     4\r\n"
            "#define KEYS_PER_GROUP      (GROUP_SIZE * KEYS_PER_THREAD)\r\n"
            "\r\n"
            "uint g_Count;\r\n"
            "uint g_Bitshift;\r\n"
            "\r\n";
        if(count != nullptr)
            sort_program_source += "RWStructuredBuffer<uint> g_CountBuffer;\r\n";
        sort_program_source +=
            "RWStructuredBuffer<uint> g_InputKeys;\r\n"
            "RWStructuredBuffer<uint> g_OutputKeys;\r\n"
            "#ifdef SORT_VALUES\r\n"
            "RWStructuredBuffer<uint> g_InputValues;\r\n"
            "RWStructuredBuffer<uint> g_OutputValues;\r\n"
            "#endif\r\n"
            "RWStructuredBuffer<uint> g_GroupHistograms;\r\n"
            "\r\n";
        if(count != nullptr)
            sort_program_source +=
                "RWStructuredBuffer<uint4> g_ArgsBuffer;\r\n"
                "RWStructuredBuffer<uint>  g_ScanCountBuffer;\r\n"
                "\r\n";
        sort_program_source +=
            "groupshared uint lds_keys[GROUP_SIZE];\r\n"
            "groupshared uint lds_scratch[GROUP_SIZE];\r\n"
            "groupshared uint lds_histogram[NUM_BINS];\r\n"
            "groupshared uint lds_scanned_histogram[NUM_BINS];\r\n"
            "\r\n"
            "uint GetCount()\r\n"
            "{\r\n";
        if(count != nullptr)
            sort_program_source += "    return min(g_CountBuffer[0], g_Count);\r\n";
        else
            sort_program_source += "    return g_Count;\r\n";
        sort_program_source +=
            "}\r\n"
            "\r\n"
            "uint GroupScan(in uint key, in uint lidx)\r\n"
            "{\r\n"
            "    lds_keys[lidx] = key;\r\n"
            "    GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "    uint stride;\r\n"
            "    for(stride = 1; stride < GROUP_SIZE; stride <<= 1)\r\n"
            "    {\r\n"
            "        if(lidx < GROUP_SIZE / (2 * stride))\r\n"
            "            lds_keys[2 * (lidx + 1) * stride - 1] += lds_keys[(2 * lidx + 1) * stride - 1];\r\n"
            "        GroupMemoryBarrierWithGroupSync();\r\n"
            "    }\r\n"
            "\r\n"
            "    if(lidx == 0)\r\n"
            "        lds_keys[GROUP_SIZE - 1] = 0;\r\n"
            "    GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "    for(stride = (GROUP_SIZE >> 1); stride > 0; stride >>= 1)\r\n"
            "    {\r\n"
            "        if(lidx < GROUP_SIZE / (2 * stride))\r\n"
            "        {\r\n"
            "            uint tmp = lds_keys[(2 * lidx + 1) * stride - 1];\r\n"
            "            lds_keys[(2 * lidx + 1) * stride - 1] = lds_keys[2 * (lidx + 1) * stride - 1];\r\n"
            "            lds_keys[2 * (lidx + 1) * stride - 1] += tmp;\r\n"
            "        }\r\n"
            "        GroupMemoryBarrierWithGroupSync();\r\n"
            "    }\r\n"
            "\r\n"
            "    return lds_keys[lidx];\r\n"
            "}\r\n"
            "\r\n"
            "[numthreads(GROUP_SIZE, 1, 1)]\r\n"
            "void Scatter(in uint lidx : SV_GroupThreadID,\r\n"
            "             in uint bidx : SV_GroupID)\r\n"
            "{\r\n"
            "    uint block_start_index = bidx * GROUP_SIZE * KEYS_PER_THREAD;\r\n"
            "    uint num_blocks = (GetCount() + KEYS_PER_GROUP - 1) / KEYS_PER_GROUP;\r\n"
            "    if(lidx < NUM_BINS)\r\n"
            "        lds_scanned_histogram[lidx] = g_GroupHistograms[num_blocks * lidx + bidx];\r\n"
            "\r\n"
            "    for(uint i = 0; i < KEYS_PER_THREAD; ++i)\r\n"
            "    {\r\n"
            "        if(lidx < NUM_BINS)\r\n"
            "            lds_histogram[lidx] = 0;\r\n"
            "\r\n"
            "        uint key_index = block_start_index + i * GROUP_SIZE + lidx;\r\n"
            "        uint key = (key_index < GetCount() ? g_InputKeys[key_index] : 0xFFFFFFFFu);\r\n"
            "#ifdef SORT_VALUES\r\n"
            "        uint value = (key_index < GetCount() ? g_InputValues[key_index] : 0);\r\n"
            "#endif\r\n"
            "\r\n"
            "        for(uint shift = 0; shift < NUM_BITS_PER_PASS; shift += 2)\r\n"
            "        {\r\n"
            "            uint bin_index = ((key >> g_Bitshift) >> shift) & 0x3;\r\n"
            "\r\n"
            "            uint local_histogram = 1u << (bin_index * 8);\r\n"
            "            uint local_histogram_scanned = GroupScan(local_histogram, lidx);\r\n"
            "            if(lidx == (GROUP_SIZE - 1))\r\n"
            "                lds_scratch[0] = local_histogram_scanned + local_histogram;\r\n"
            "            GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "            local_histogram = lds_scratch[0];\r\n"
            "            local_histogram = (local_histogram << 8) +\r\n"
            "                              (local_histogram << 16) +\r\n"
            "                              (local_histogram << 24);\r\n"
            "            local_histogram_scanned += local_histogram;\r\n"
            "\r\n"
            "            uint offset = (local_histogram_scanned >> (bin_index * 8)) & 0xFFu;\r\n"
            "            lds_keys[offset] = key;\r\n"
            "            GroupMemoryBarrierWithGroupSync();\r\n"
            "            key = lds_keys[lidx];\r\n"
            "\r\n"
            "#ifdef SORT_VALUES\r\n"
            "            GroupMemoryBarrierWithGroupSync();\r\n"
            "            lds_keys[offset] = value;\r\n"
            "            GroupMemoryBarrierWithGroupSync();\r\n"
            "            value = lds_keys[lidx];\r\n"
            "            GroupMemoryBarrierWithGroupSync();\r\n"
            "#endif\r\n"
            "        }\r\n"
            "\r\n"
            "        uint bin_index = (key >> g_Bitshift) & 0xFu;\r\n"
            "        InterlockedAdd(lds_histogram[bin_index], 1);\r\n"
            "        GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "        uint histogram_value = GroupScan(lidx < NUM_BINS ? lds_histogram[lidx] : 0, lidx);\r\n"
            "        if(lidx < NUM_BINS)\r\n"
            "            lds_scratch[lidx] = histogram_value;\r\n"
            "\r\n"
            "        uint global_offset = lds_scanned_histogram[bin_index];\r\n"
            "        GroupMemoryBarrierWithGroupSync();\r\n"
            "        uint local_offset = lidx - lds_scratch[bin_index];\r\n"
            "\r\n"
            "        if(global_offset + local_offset < GetCount())\r\n"
            "        {\r\n"
            "            g_OutputKeys[global_offset + local_offset] = key;\r\n"
            "#ifdef SORT_VALUES\r\n"
            "            g_OutputValues[global_offset + local_offset] = value;\r\n"
            "#endif\r\n"
            "        }\r\n"
            "        GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "        if(lidx < NUM_BINS)\r\n"
            "            lds_scanned_histogram[lidx] += lds_histogram[lidx];\r\n"
            "    }\r\n"
            "}\r\n"
            "\r\n"
            "[numthreads(GROUP_SIZE, 1, 1)]\r\n"
            "void BitHistogram(in uint gidx : SV_DispatchThreadID,\r\n"
            "                  in uint lidx : SV_GroupThreadID,\r\n"
            "                  in uint bidx : SV_GroupID)\r\n"
            "{\r\n"
            "    if(lidx < NUM_BINS)\r\n"
            "        lds_histogram[lidx] = 0;\r\n"
            "    GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "    for(uint i = 0; i < KEYS_PER_THREAD; ++i)\r\n"
            "    {\r\n"
            "        uint key_index = gidx * KEYS_PER_THREAD + i;\r\n"
            "        if(key_index >= GetCount()) break;  // out of bounds\r\n"
            "        uint bin_index = (g_InputKeys[key_index] >> g_Bitshift) & 0xFu;\r\n"
            "        InterlockedAdd(lds_histogram[bin_index], 1);\r\n"
            "    }\r\n"
            "    GroupMemoryBarrierWithGroupSync();\r\n"
            "\r\n"
            "    if(lidx < NUM_BINS)\r\n"
            "    {\r\n"
            "        uint num_blocks = (GetCount() + KEYS_PER_GROUP - 1) / KEYS_PER_GROUP;\r\n"
            "        g_GroupHistograms[num_blocks * lidx + bidx] = lds_histogram[lidx];\r\n"
            "    }\r\n"
            "}\r\n";
        if(count != nullptr)
            sort_program_source +=
                "\r\n"
                "[numthreads(1, 1, 1)]\r\n"
                "void GenerateDispatches()\r\n"
                "{\r\n"
                "    uint num_groups = (GetCount() + KEYS_PER_GROUP - 1) / KEYS_PER_GROUP;\r\n"
                "    g_ArgsBuffer[0] = uint4(num_groups, 1, 1, 0);\r\n"
                "    g_ScanCountBuffer[0] = NUM_BINS * num_groups;\r\n"
                "}\r\n";
        SortKernels &sort_kernels = sort_kernels_[key];
        GfxProgramDesc sort_program_desc = {};
        sort_program_desc.cs = sort_program_source.c_str();
        char const *sort_values_defines[] = { "SORT_VALUES" };
        sort_kernels.sort_program_ = createProgram(sort_program_desc, "gfx_SortProgram", nullptr, nullptr, 0);
        sort_kernels.histogram_kernel_ = createComputeKernel(sort_kernels.sort_program_, "BitHistogram", nullptr, 0);
        sort_kernels.scatter_kernel_ = createComputeKernel(sort_kernels.sort_program_, "Scatter", sort_values_defines, sort_values ? 1 : 0);
        if(count != nullptr)
            sort_kernels.args_kernel_ = createComputeKernel(sort_kernels.sort_program_, "GenerateDispatches", nullptr, 0);
        return sort_kernels;
    }

    void populateRootConstants(Program const &program, Program::Parameters const &parameters, Kernel::Parameter &parameter, uint32_t *root_constants, bool force_update_parameter = false)
    {
        memset(root_constants, 0, parameter.variable_size_);
        static uint64_t const dispatch_id_parameter = Hash("gfx_DispatchID");
        for(uint32_t i = 0; i < parameter.variable_count_; ++i)
        {
            Kernel::Parameter::Variable &variable = parameter.variables_[i];
            if(force_update_parameter || (variable.parameter_ == nullptr && variable.parameter_id_ != dispatch_id_parameter))
            {
                Program::Parameters::const_iterator const it = parameters.find(variable.parameter_id_);
                if(it != parameters.end()) variable.parameter_ = &(*it).second;
            }
            if(variable.parameter_ == nullptr) continue;
            if(variable.parameter_->type_ != Program::Parameter::kType_Constants)
            {
                if(variable.id_ != variable.parameter_->id_)
                    GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected constants", variable.parameter_->getTypeName(), variable.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                variable.id_ = variable.parameter_->id_;
                continue;   // invalid parameter
            }
            memcpy(&root_constants[variable.data_start_ / sizeof(uint32_t)], variable.parameter_->data_.constants_, GFX_MIN(variable.data_size_, variable.parameter_->data_size_));
            variable.id_ = variable.parameter_->id_;
        }
    }

    void initDescriptorParameter(Kernel const &kernel, Program const &program, bool const invalidate_descriptors, Kernel::Parameter &parameter, uint32_t &descriptor_slot)
    {
        if(parameter.parameter_ == nullptr && !(parameter.type_ == Kernel::Parameter::kType_ConstantBuffer && parameter.variable_size_ > 0))
        {
            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found invalid parameter");
            return;
        }
        bool const invalidate_descriptor = parameter.parameter_ != nullptr && (invalidate_descriptors || parameter.id_ != parameter.parameter_->id_);
        if(parameter.parameter_ != nullptr) parameter.id_ = parameter.parameter_->id_;
        switch(parameter.type_)
        {
        case Kernel::Parameter::kType_Buffer:
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC dummy_srv_desc = {};
                dummy_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dummy_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                dummy_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                    if(j >= parameter.parameter_->data_.buffer_.buffer_count ||
                        parameter.parameter_->type_ != Program::Parameter::kType_Buffer)
                    {
                        if(!invalidate_descriptor) continue;
                        if(j == 0 && parameter.parameter_->type_ != Program::Parameter::kType_Buffer)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a buffer object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
                    else
                    {
                        GfxBuffer const &buffer = parameter.parameter_->data_.buffer_.buffers_[j];
                        if(!buffer_handles_.has_handle(buffer.handle))
                        {
                            if(buffer.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid buffer object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            if(!invalidate_descriptor) continue;    // already up to date
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;  // user set an invalid buffer object
                        }
                        if(buffer.cpu_access == kGfxCpuAccess_Read)
                        {
                            if(!invalidate_descriptor) continue;
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind buffer object with read CPU access as a shader resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;  // invalid buffer object for shader SRV
                        }
                        if(buffer.stride == 0)
                        {
                            if(!invalidate_descriptor) continue;
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind buffer object with a stride of 0 as a shader resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;  // invalid buffer stride
                        }
                        if(buffer.size < buffer.stride)
                        {
                            if(!invalidate_descriptor) continue;
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;  // invalid buffer size
                        }
                        Buffer &gfx_buffer = buffers_[buffer];
                        SetObjectName(gfx_buffer, buffer.name);
                        if(buffer.cpu_access == kGfxCpuAccess_None)
                            transitionResource(gfx_buffer, GetShaderVisibleResourceState(kernel), kTransitionType_Implicit);
                        if(!invalidate_descriptor) continue;    // already up to date
                        if(buffer.stride != GFX_ALIGN(buffer.stride, 4))
                            GFX_PRINTLN("Warning: Encountered a buffer stride of %u that isn't 4-byte aligned for parameter `%s' of program `%s/%s'; is this intentional?", buffer.stride, parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv_desc.Buffer.FirstElement = gfx_buffer.data_offset_ / buffer.stride;
                        srv_desc.Buffer.NumElements = (uint32_t)(buffer.size / buffer.stride);
                        srv_desc.Buffer.StructureByteStride = buffer.stride;
                        device_->CreateShaderResourceView(gfx_buffer.resource_, &srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
            }
            break;
        case Kernel::Parameter::kType_RWBuffer:
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC dummy_uav_desc = {};
                dummy_uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dummy_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                    if(j >= parameter.parameter_->data_.buffer_.buffer_count ||
                        parameter.parameter_->type_ != Program::Parameter::kType_Buffer)
                    {
                        if(!invalidate_descriptor) continue;
                        if(j == 0 && parameter.parameter_->type_ != Program::Parameter::kType_Buffer)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a buffer object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
                    else
                    {
                        GfxBuffer const &buffer = parameter.parameter_->data_.buffer_.buffers_[j];
                        if(!buffer_handles_.has_handle(buffer.handle))
                        {
                            if(buffer.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid buffer object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            if(!invalidate_descriptor) continue;    // already up to date
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;  // user set an invalid buffer object
                        }
                        if(buffer.cpu_access != kGfxCpuAccess_None)
                        {
                            if(!invalidate_descriptor) continue;
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind buffer object with read/write CPU access as a RW shader resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;  // invalid buffer object for shader UAV
                        }
                        if(buffer.stride == 0)
                        {
                            if(!invalidate_descriptor) continue;
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind buffer object with a stride of 0 as a RW shader resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;  // invalid buffer stride
                        }
                        if(buffer.size < buffer.stride)
                        {
                            if(!invalidate_descriptor) continue;
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;  // invalid buffer size
                        }
                        Buffer &gfx_buffer = buffers_[buffer];
                        SetObjectName(gfx_buffer, buffer.name);
                        D3D12_RESOURCE_DESC const resource_desc = gfx_buffer.resource_->GetDesc();
                        if(!((resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0))
                        {
                            if(!invalidate_descriptor) continue;    // invalid resource use
                            GFX_SET_ERROR(kGfxResult_InvalidOperation, "Cannot re-create interop buffer objects with different usage flag(s)");
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid operation
                        }
                        transitionResource(gfx_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        if(!invalidate_descriptor) continue;    // already up to date
                        if(buffer.stride != GFX_ALIGN(buffer.stride, 4))
                            GFX_PRINTLN("Warning: Encountered a buffer stride of %u that isn't 4-byte aligned for parameter `%s' of program `%s/%s'; is this intentional?", buffer.stride, parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
                        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                        if(parameter.raw_access_)
                        {
                            // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_buffer_uav_flags
                            uav_desc.Buffer.FirstElement = gfx_buffer.data_offset_ / 4;
                            uav_desc.Buffer.NumElements = (uint32_t)(buffer.size / 4);
                            uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
                            uav_desc.Buffer.StructureByteStride = 0;
                            uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                        }
                        else
                        {
                            uav_desc.Buffer.FirstElement = gfx_buffer.data_offset_ / buffer.stride;
                            uav_desc.Buffer.NumElements = (uint32_t)(buffer.size / buffer.stride);
                            uav_desc.Buffer.StructureByteStride = buffer.stride;
                        }
                        device_->CreateUnorderedAccessView(gfx_buffer.resource_, nullptr, &uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
            }
            break;
        case Kernel::Parameter::kType_Texture2D:
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC dummy_srv_desc = {};
                dummy_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dummy_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                dummy_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                parameter.bound_textures_.resize(parameter.descriptor_count_);
                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                    if(j >= parameter.parameter_->data_.image_.texture_count ||
                        parameter.parameter_->type_ != Program::Parameter::kType_Image)
                    {
                        if(!invalidate_descriptor) continue;    // already up to date
                        if(j == 0 && parameter.parameter_->type_ != Program::Parameter::kType_Image)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a texture object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
                    else
                    {
                        GfxTexture const &texture = parameter.parameter_->data_.image_.textures_[j];
                        if(!texture_handles_.has_handle(texture.handle))
                        {
                            if(texture.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid texture object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            if(!invalidate_descriptor && parameter.bound_textures_[j] == nullptr) continue;    // already up to date
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            parameter.bound_textures_[j] = nullptr; // invalidate cached pointer
                            continue;   // user set an invalid texture object
                        }
                        if(!texture.is2D())
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind non-2D texture object as a 2D sampler resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid texture object for shader SRV
                        }
                        Texture &gfx_texture = textures_[texture];
                        SetObjectName(gfx_texture, texture.name);
                        uint32_t const mip_level = GetMipLevel(*parameter.parameter_, j);
                        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
                        if(mip_level >= (uint32_t)resource_desc.MipLevels)
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind out of bounds mip level of 2D texture object for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // out of bounds mip level
                        }
                        transitionResource(gfx_texture, GetShaderVisibleResourceState(kernel), kTransitionType_Implicit);
                        if(!invalidate_descriptor && gfx_texture.resource_ == parameter.bound_textures_[j])
                            continue;    // already up to date
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                        srv_desc.Format = GetCBVSRVUAVFormat(resource_desc.Format);
                        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv_desc.Texture2D.MostDetailedMip = mip_level;
                        srv_desc.Texture2D.MipLevels = 0xFFFFFFFFu; // select all mipmaps from MostDetailedMip on down to least detailed
                        device_->CreateShaderResourceView(gfx_texture.resource_, &srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                        parameter.bound_textures_[j] = gfx_texture.resource_;   // cache resource pointer
                    }
            }
            break;
        case Kernel::Parameter::kType_RWTexture2D:
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC dummy_uav_desc = {};
                dummy_uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dummy_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                parameter.bound_textures_.resize(parameter.descriptor_count_);
                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                    if(j >= parameter.parameter_->data_.image_.texture_count ||
                        parameter.parameter_->type_ != Program::Parameter::kType_Image)
                    {
                        if(!invalidate_descriptor) continue;    // already up to date
                        if(j == 0 && parameter.parameter_->type_ != Program::Parameter::kType_Image)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a texture object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
                    else
                    {
                        GfxTexture const &texture = parameter.parameter_->data_.image_.textures_[j];
                        if(!texture_handles_.has_handle(texture.handle))
                        {
                            if(texture.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid texture object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            if(!invalidate_descriptor && parameter.bound_textures_[j] == nullptr) continue;    // already up to date
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            parameter.bound_textures_[j] = nullptr; // invalidate cached pointer
                            continue;   // user set an invalid texture object
                        }
                        if(!texture.is2D())
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind non-2D texture object as a 2D image resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid texture object for shader UAV
                        }
                        Texture &gfx_texture = textures_[texture];
                        SetObjectName(gfx_texture, texture.name);
                        uint32_t const mip_level = GetMipLevel(*parameter.parameter_, j);
                        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
                        if(mip_level >= (uint32_t)resource_desc.MipLevels)
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind out of bounds mip level of 2D texture object for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // out of bounds mip level
                        }
                        if(!((resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0))
                        {
                            if(!invalidate_descriptor) continue;    // invalid resource use
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid operation
                        }
                        transitionResource(gfx_texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        if(!invalidate_descriptor && gfx_texture.resource_ == parameter.bound_textures_[j])
                            continue;    // already up to date
                        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
                        uav_desc.Format = GetUAVFormat(resource_desc.Format);
                        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                        uav_desc.Texture2D.MipSlice = mip_level;
                        device_->CreateUnorderedAccessView(gfx_texture.resource_, nullptr, &uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                        parameter.bound_textures_[j] = gfx_texture.resource_;   // cache resource pointer
                    }
            }
            break;
        case Kernel::Parameter::kType_Texture2DArray:
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC dummy_srv_desc = {};
                dummy_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dummy_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                dummy_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                parameter.bound_textures_.resize(parameter.descriptor_count_);
                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                    if(j >= parameter.parameter_->data_.image_.texture_count ||
                        parameter.parameter_->type_ != Program::Parameter::kType_Image)
                    {
                        if(!invalidate_descriptor) continue;    // already up to date
                        if(j == 0 && parameter.parameter_->type_ != Program::Parameter::kType_Image)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a texture object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
                    else
                    {
                        GfxTexture const &texture = parameter.parameter_->data_.image_.textures_[j];
                        if(!texture_handles_.has_handle(texture.handle))
                        {
                            if(texture.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid texture object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            if(!invalidate_descriptor && parameter.bound_textures_[j] == nullptr) continue;    // already up to date
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            parameter.bound_textures_[j] = nullptr; // invalidate cached pointer
                            continue;   // user set an invalid texture object
                        }
                        if(!texture.is2DArray())
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind non-2D array texture object as a 2D sampler array resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid texture object for shader SRV
                        }
                        Texture &gfx_texture = textures_[texture];
                        SetObjectName(gfx_texture, texture.name);
                        uint32_t const mip_level = GetMipLevel(*parameter.parameter_, j);
                        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
                        if(mip_level >= (uint32_t)resource_desc.MipLevels)
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind out of bounds mip level of 2D array texture object for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // out of bounds mip level
                        }
                        transitionResource(gfx_texture, GetShaderVisibleResourceState(kernel), kTransitionType_Implicit);
                        if(!invalidate_descriptor && gfx_texture.resource_ == parameter.bound_textures_[j])
                            continue;    // already up to date
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                        srv_desc.Format = GetCBVSRVUAVFormat(resource_desc.Format);
                        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv_desc.Texture2DArray.MostDetailedMip = mip_level;
                        srv_desc.Texture2DArray.MipLevels = 0xFFFFFFFFu;    // select all mipmaps from MostDetailedMip on down to least detailed
                        srv_desc.Texture2DArray.ArraySize = resource_desc.DepthOrArraySize;
                        device_->CreateShaderResourceView(gfx_texture.resource_, &srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                        parameter.bound_textures_[j] = gfx_texture.resource_;   // cache resource pointer
                    }
            }
            break;
        case Kernel::Parameter::kType_RWTexture2DArray:
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC dummy_uav_desc = {};
                dummy_uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dummy_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                parameter.bound_textures_.resize(parameter.descriptor_count_);
                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                    if(j >= parameter.parameter_->data_.image_.texture_count ||
                        parameter.parameter_->type_ != Program::Parameter::kType_Image)
                    {
                        if(!invalidate_descriptor) continue;    // already up to date
                        if(j == 0 && parameter.parameter_->type_ != Program::Parameter::kType_Image)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a texture object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
                    else
                    {
                        GfxTexture const &texture = parameter.parameter_->data_.image_.textures_[j];
                        if(!texture_handles_.has_handle(texture.handle))
                        {
                            if(texture.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid texture object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            if(!invalidate_descriptor && parameter.bound_textures_[j] == nullptr) continue;    // already up to date
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            parameter.bound_textures_[j] = nullptr; // invalidate cached pointer
                            continue;   // user set an invalid texture object
                        }
                        if(!texture.is2DArray() && !texture.isCube())
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind non-2D array texture object as a 2D image array resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid texture object for shader UAV
                        }
                        Texture &gfx_texture = textures_[texture];
                        SetObjectName(gfx_texture, texture.name);
                        uint32_t const mip_level = GetMipLevel(*parameter.parameter_, j);
                        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
                        if(mip_level >= (uint32_t)resource_desc.MipLevels)
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind out of bounds mip level of 2D array texture object for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // out of bounds mip level
                        }
                        if(!((resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0))
                        {
                            if(!invalidate_descriptor) continue;    // invalid resource use
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid operation
                        }
                        transitionResource(gfx_texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        if(!invalidate_descriptor && gfx_texture.resource_ == parameter.bound_textures_[j])
                            continue;    // already up to date
                        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
                        uav_desc.Format = GetUAVFormat(resource_desc.Format);
                        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                        uav_desc.Texture2DArray.MipSlice = mip_level;
                        uav_desc.Texture2DArray.ArraySize = resource_desc.DepthOrArraySize;
                        device_->CreateUnorderedAccessView(gfx_texture.resource_, nullptr, &uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                        parameter.bound_textures_[j] = gfx_texture.resource_;   // cache resource pointer
                    }
            }
            break;
        case Kernel::Parameter::kType_Texture3D:
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC dummy_srv_desc = {};
                dummy_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dummy_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                dummy_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                parameter.bound_textures_.resize(parameter.descriptor_count_);
                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                    if(j >= parameter.parameter_->data_.image_.texture_count ||
                        parameter.parameter_->type_ != Program::Parameter::kType_Image)
                    {
                        if(!invalidate_descriptor) continue;    // already up to date
                        if(j == 0 && parameter.parameter_->type_ != Program::Parameter::kType_Image)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a texture object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
                    else
                    {
                        GfxTexture const &texture = parameter.parameter_->data_.image_.textures_[j];
                        if(!texture_handles_.has_handle(texture.handle))
                        {
                            if(texture.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid texture object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            if(!invalidate_descriptor && parameter.bound_textures_[j] == nullptr) continue;    // already up to date
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            parameter.bound_textures_[j] = nullptr; // invalidate cached pointer
                            continue;   // user set an invalid texture object
                        }
                        if(!texture.is3D())
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind non-3D texture object as a 3D sampler resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid texture object for shader SRV
                        }
                        Texture &gfx_texture = textures_[texture];
                        SetObjectName(gfx_texture, texture.name);
                        uint32_t const mip_level = GetMipLevel(*parameter.parameter_, j);
                        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
                        if(mip_level >= (uint32_t)resource_desc.MipLevels)
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind out of bounds mip level of 3D texture object for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // out of bounds mip level
                        }
                        transitionResource(gfx_texture, GetShaderVisibleResourceState(kernel), kTransitionType_Implicit);
                        if(!invalidate_descriptor && gfx_texture.resource_ == parameter.bound_textures_[j])
                            continue;    // already up to date
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                        srv_desc.Format = GetCBVSRVUAVFormat(resource_desc.Format);
                        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv_desc.Texture3D.MostDetailedMip = mip_level;
                        srv_desc.Texture3D.MipLevels = 0xFFFFFFFFu; // select all mipmaps from MostDetailedMip on down to least detailed
                        device_->CreateShaderResourceView(gfx_texture.resource_, &srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                        parameter.bound_textures_[j] = gfx_texture.resource_;   // cache resource pointer
                    }
            }
            break;
        case Kernel::Parameter::kType_RWTexture3D:
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC dummy_uav_desc = {};
                dummy_uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dummy_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                parameter.bound_textures_.resize(parameter.descriptor_count_);
                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                    if(j >= parameter.parameter_->data_.image_.texture_count ||
                        parameter.parameter_->type_ != Program::Parameter::kType_Image)
                    {
                        if(!invalidate_descriptor) continue;    // already up to date
                        if(j == 0 && parameter.parameter_->type_ != Program::Parameter::kType_Image)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a texture object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
                    else
                    {
                        GfxTexture const &texture = parameter.parameter_->data_.image_.textures_[j];
                        if(!texture_handles_.has_handle(texture.handle))
                        {
                            if(texture.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid texture object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            if(!invalidate_descriptor && parameter.bound_textures_[j] == nullptr) continue;    // already up to date
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            parameter.bound_textures_[j] = nullptr; // invalidate cached pointer
                            continue;   // user set an invalid texture object
                        }
                        if(!texture.is3D())
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind non-3D texture object as a 3D image resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid texture object for shader UAV
                        }
                        Texture &gfx_texture = textures_[texture];
                        SetObjectName(gfx_texture, texture.name);
                        uint32_t const mip_level = GetMipLevel(*parameter.parameter_, j);
                        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
                        if(mip_level >= (uint32_t)resource_desc.MipLevels)
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind out of bounds mip level of 3D texture object for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // out of bounds mip level
                        }
                        if(!((resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0))
                        {
                            if(!invalidate_descriptor) continue;    // invalid resource use
                            device_->CreateUnorderedAccessView(nullptr, nullptr, &dummy_uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid operation
                        }
                        transitionResource(gfx_texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        if(!invalidate_descriptor && gfx_texture.resource_ == parameter.bound_textures_[j])
                            continue;    // already up to date
                        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
                        uav_desc.Format = GetUAVFormat(resource_desc.Format);
                        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                        uav_desc.Texture3D.MipSlice = mip_level;
                        uav_desc.Texture3D.WSize = resource_desc.DepthOrArraySize;
                        device_->CreateUnorderedAccessView(gfx_texture.resource_, nullptr, &uav_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                        parameter.bound_textures_[j] = gfx_texture.resource_;   // cache resource pointer
                    }
            }
            break;
        case Kernel::Parameter::kType_TextureCube:
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC dummy_srv_desc = {};
                dummy_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dummy_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                dummy_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                parameter.bound_textures_.resize(parameter.descriptor_count_);
                for(uint32_t j = 0; j < parameter.descriptor_count_; ++j)
                    if(j >= parameter.parameter_->data_.image_.texture_count ||
                        parameter.parameter_->type_ != Program::Parameter::kType_Image)
                    {
                        if(!invalidate_descriptor) continue;    // already up to date
                        if(j == 0 && parameter.parameter_->type_ != Program::Parameter::kType_Image)
                            GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected a texture object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                    }
                    else
                    {
                        GfxTexture const &texture = parameter.parameter_->data_.image_.textures_[j];
                        if(!texture_handles_.has_handle(texture.handle))
                        {
                            if(texture.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid texture object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            if(!invalidate_descriptor && parameter.bound_textures_[j] == nullptr) continue;    // already up to date
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            parameter.bound_textures_[j] = nullptr; // invalidate cached pointer
                            continue;   // user set an invalid texture object
                        }
                        if(!texture.isCube() && (!texture.is2DArray() || texture.getWidth() != texture.getHeight() || texture.getDepth() != 6))
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind non-cube texture object as a cubemap sampler resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // invalid texture object for shader SRV
                        }
                        Texture &gfx_texture = textures_[texture];
                        SetObjectName(gfx_texture, texture.name);
                        uint32_t const mip_level = GetMipLevel(*parameter.parameter_, j);
                        D3D12_RESOURCE_DESC const resource_desc = gfx_texture.resource_->GetDesc();
                        if(mip_level >= (uint32_t)resource_desc.MipLevels)
                        {
                            if(!invalidate_descriptor) continue;    // already up to date
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind out of bounds mip level of cube texture object for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            device_->CreateShaderResourceView(nullptr, &dummy_srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                            continue;   // out of bounds mip level
                        }
                        transitionResource(gfx_texture, GetShaderVisibleResourceState(kernel), kTransitionType_Implicit);
                        if(!invalidate_descriptor && gfx_texture.resource_ == parameter.bound_textures_[j])
                            continue;    // already up to date
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                        srv_desc.Format = GetCBVSRVUAVFormat(resource_desc.Format);
                        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv_desc.TextureCube.MostDetailedMip = mip_level;
                        srv_desc.TextureCube.MipLevels = 0xFFFFFFFFu;   // select all mipmaps from MostDetailedMip on down to least detailed
                        device_->CreateShaderResourceView(gfx_texture.resource_, &srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_ + j));
                        parameter.bound_textures_[j] = gfx_texture.resource_;   // cache resource pointer
                    }
            }
            break;
        case Kernel::Parameter::kType_AccelerationStructure:
            {
                if(parameter.parameter_->type_ != Program::Parameter::kType_AccelerationStructure)
                {
                    GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected an acceleration structure object", parameter.parameter_->getTypeName(), parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                    descriptor_slot = dummy_descriptors_[parameter.type_];
                    freeDescriptor(parameter.descriptor_slot_);
                    parameter.descriptor_slot_ = 0xFFFFFFFFu;
                    break;  // user set an unrelated parameter type
                }
                GfxAccelerationStructure const &acceleration_structure = parameter.parameter_->data_.acceleration_structure_.bvh_;
                if(!acceleration_structure_handles_.has_handle(acceleration_structure.handle))
                {
                    if(acceleration_structure.handle != 0)
                        GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid acceleration structure object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                    descriptor_slot = dummy_descriptors_[parameter.type_];
                    freeDescriptor(parameter.descriptor_slot_);
                    parameter.descriptor_slot_ = 0xFFFFFFFFu;
                    break;  // user set an invalid buffer object
                }
                AccelerationStructure const &gfx_acceleration_structure = acceleration_structures_[acceleration_structure];
                const_cast<Program::Parameter *>(parameter.parameter_)->data_.acceleration_structure_.bvh_buffer_ = gfx_acceleration_structure.bvh_buffer_;
                if(!gfx_acceleration_structure.bvh_buffer_)
                {
                    descriptor_slot = dummy_descriptors_[parameter.type_];
                    freeDescriptor(parameter.descriptor_slot_);
                    parameter.descriptor_slot_ = 0xFFFFFFFFu;
                    break;  // acceleration structure hasn't been built yet
                }
                GFX_ASSERT(buffer_handles_.has_handle(gfx_acceleration_structure.bvh_buffer_.handle));
                Buffer &buffer = buffers_[gfx_acceleration_structure.bvh_buffer_];
                SetObjectName(buffer, acceleration_structure.name);
                if(buffer_handles_.has_handle(raytracing_scratch_buffer_.handle))
                    transitionResource(buffers_[raytracing_scratch_buffer_], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kTransitionType_Implicit);
                GFX_ASSERT(*buffer.resource_state_ == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                if(!invalidate_descriptor) break;   // already up to date
                D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv_desc.RaytracingAccelerationStructure.Location = buffer.resource_->GetGPUVirtualAddress() + buffer.data_offset_;
                device_->CreateShaderResourceView(nullptr, &srv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_));
            }
            break;
        case Kernel::Parameter::kType_ConstantBuffer:
            {
                void *data = nullptr;
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
                if(parameter.variable_size_ > 0)
                {
                    cbv_desc.BufferLocation = allocateConstantMemory(parameter.variable_size_, data);
                    if(data != nullptr) populateRootConstants(program, program.parameters_, parameter, (uint32_t *)data);
                    cbv_desc.SizeInBytes = GFX_ALIGN(parameter.variable_size_, 256);
                }
                else
                {
                    if(parameter.parameter_->type_ == Program::Parameter::kType_Constants)
                    {
                        cbv_desc.BufferLocation = allocateConstantMemory(parameter.parameter_->data_size_, data);
                        if(data != nullptr) memcpy(data, parameter.parameter_->data_.constants_, parameter.parameter_->data_size_);
                        cbv_desc.SizeInBytes = GFX_ALIGN(parameter.parameter_->data_size_, 256);
                    }
                    else if(parameter.parameter_->type_ == Program::Parameter::kType_Buffer)
                    {
                        if(parameter.parameter_->data_.buffer_.buffer_count > 1)
                        {
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found several buffer objects for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            break;  // user set an invalid buffer object
                        }
                        if(parameter.parameter_->data_.buffer_.buffer_count < 1)
                        {
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found no buffer object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str (),    program.file_name_.c_str());
                            break;  // user set an invalid buffer object
                        }
                        GfxBuffer const &buffer = parameter.parameter_->data_.buffer_.buffers_[0];
                        if(!buffer_handles_.has_handle(buffer.handle))
                        {
                            if(buffer.handle != 0)
                                GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Found invalid buffer object for parameter `%s' of program `%s/%s'; cannot bind to pipeline", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            descriptor_slot = dummy_descriptors_[parameter.type_];
                            freeDescriptor(parameter.descriptor_slot_);
                            parameter.descriptor_slot_ = 0xFFFFFFFFu;
                            break;  // user set an invalid buffer object
                        }
                        if(buffer.cpu_access != kGfxCpuAccess_Write)
                        {
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind buffer object that does not have write CPU access as a constant shader resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                            descriptor_slot = dummy_descriptors_[parameter.type_];
                            freeDescriptor(parameter.descriptor_slot_);
                            parameter.descriptor_slot_ = 0xFFFFFFFFu;
                            break;  // user set an invalid buffer object
                        }
                        if(buffer.size > 0xFFFFFFFFull)
                        {
                            GFX_PRINT_ERROR(kGfxResult_InvalidOperation, "Cannot bind buffer object that's larger than 4GiB as a constant shader resource for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str (),  program.file_path_.c_str(), program.file_name_.c_str());
                            descriptor_slot = dummy_descriptors_[parameter.type_];
                            freeDescriptor(parameter.descriptor_slot_);
                            parameter.descriptor_slot_ = 0xFFFFFFFFu;
                            break;  // constant buffer is too large
                        }
                        Buffer &gfx_buffer = buffers_[buffer];
                        SetObjectName(gfx_buffer, buffer.name);
                        GFX_ASSERT(*gfx_buffer.resource_state_ == D3D12_RESOURCE_STATE_GENERIC_READ);
                        GFX_ASSERT(gfx_buffer.data_offset_ == GFX_ALIGN(gfx_buffer.data_offset_, 256));
                        cbv_desc.BufferLocation = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
                        cbv_desc.SizeInBytes = GFX_ALIGN((uint32_t)buffer.size, 256);
                    }
                    else
                    {
                        GFX_PRINT_ERROR(kGfxResult_InvalidParameter, "Found unrelated type `%s' for parameter `%s' of program `%s/%s'; expected constant or buffer object", parameter.parameter_->getTypeName(),   parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                        descriptor_slot = dummy_descriptors_[parameter.type_];
                        freeDescriptor(parameter.descriptor_slot_);
                        parameter.descriptor_slot_ = 0xFFFFFFFFu;
                        break;  // user set an unrelated parameter type
                    }
                }
                if(cbv_desc.BufferLocation == 0)
                {
                    if(parameter.parameter_ != nullptr)
                        GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to allocate constant memory for parameter `%s' of program `%s/%s'", parameter.parameter_->name_.c_str(), program.file_path_.c_str(), program.file_name_.c_str());
                    descriptor_slot = dummy_descriptors_[parameter.type_];
                    freeDescriptor(parameter.descriptor_slot_);
                    parameter.descriptor_slot_ = 0xFFFFFFFFu;
                    break;  // failed to allocate constant memory
                }
                device_->CreateConstantBufferView(&cbv_desc, descriptors_.getCPUHandle(parameter.descriptor_slot_));
            }
            break;
        default:
            GFX_ASSERT(0);  // missing implementation
            break;
        }
    }

    void bindDrawIdBuffer()
    {
        GFX_ASSERT(command_list_ != nullptr);
        if(!draw_id_buffer_) return;    // no need for drawID
        GFX_ASSERT(buffer_handles_.has_handle(draw_id_buffer_.handle));
        Buffer &gfx_buffer = buffers_[draw_id_buffer_];
        SetObjectName(buffers_[draw_id_buffer_], draw_id_buffer_.name);
        D3D12_VERTEX_BUFFER_VIEW
        vbv_desc                = {};
        vbv_desc.BufferLocation = gfx_buffer.resource_->GetGPUVirtualAddress() + gfx_buffer.data_offset_;
        vbv_desc.SizeInBytes    = (uint32_t)draw_id_buffer_.size;
        vbv_desc.StrideInBytes  = sizeof(uint32_t);
        command_list_->IASetVertexBuffers(1, 1, &vbv_desc);
    }

    GfxResult populateDrawIdBuffer(uint32_t args_count)
    {
        uint64_t draw_id_buffer_size = args_count * sizeof(uint32_t);
        if(draw_id_buffer_size > draw_id_buffer_.size)
        {
            GFX_TRY(destroyBuffer(draw_id_buffer_));
            draw_id_buffer_size += ((draw_id_buffer_size + 2) >> 1);
            draw_id_buffer_size = GFX_ALIGN(draw_id_buffer_size, 65536);
            std::vector<uint32_t> draw_ids((size_t)(draw_id_buffer_size / sizeof(uint32_t)));
            for(size_t i = 0; i < draw_ids.size(); ++i) draw_ids[i] = static_cast<uint32_t>(i);
            draw_id_buffer_ = createBuffer(draw_id_buffer_size, draw_ids.data(), kGfxCpuAccess_None, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            if(!draw_id_buffer_)
                return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to create the drawID buffer");
            draw_id_buffer_.setName("gfx_DrawIDBuffer");
            force_install_draw_id_buffer_ = true;
        }
        if(force_install_draw_id_buffer_)
        {
            bindDrawIdBuffer(); // bind drawID buffer
            force_install_draw_id_buffer_ = false;
        }
        return kGfxResult_NoError;
    }

    void populateDummyDescriptors()
    {
        GFX_ASSERT(ARRAYSIZE(dummy_descriptors_) == Kernel::Parameter::kType_Count);
        for(uint32_t i = 0; i < ARRAYSIZE(dummy_descriptors_); ++i)
            switch(i)
            {
            case Kernel::Parameter::kType_Buffer:
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    srv_desc.Buffer.StructureByteStride = sizeof(uint32_t);
                    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    device_->CreateShaderResourceView(nullptr, &srv_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_RWBuffer:
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
                    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    uav_desc.Buffer.StructureByteStride = sizeof(uint32_t);
                    device_->CreateUnorderedAccessView(nullptr, nullptr, &uav_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_Texture2D:
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    device_->CreateShaderResourceView(nullptr, &srv_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_RWTexture2D:
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
                    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    device_->CreateUnorderedAccessView(nullptr, nullptr, &uav_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_Texture2DArray:
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    device_->CreateShaderResourceView(nullptr, &srv_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_RWTexture2DArray:
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
                    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    device_->CreateUnorderedAccessView(nullptr, nullptr, &uav_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_Texture3D:
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    device_->CreateShaderResourceView(nullptr, &srv_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_RWTexture3D:
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
                    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                    device_->CreateUnorderedAccessView(nullptr, nullptr, &uav_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_TextureCube:
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    device_->CreateShaderResourceView(nullptr, &srv_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_AccelerationStructure:
                if(dxr_device_ != nullptr)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    device_->CreateShaderResourceView(nullptr, &srv_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_Constants:
                {
                    freeDescriptor(dummy_descriptors_[i]);
                    dummy_descriptors_[i] = 0xFFFFFFFFu;
                }
                break;
            case Kernel::Parameter::kType_ConstantBuffer:
                {
                    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
                    device_->CreateConstantBufferView(&cbv_desc, descriptors_.getCPUHandle(dummy_descriptors_[i]));
                }
                break;
            case Kernel::Parameter::kType_Sampler:
                break;  // the sampler dummy descriptor is (re-)populated every time the sampler descriptor heap is resized
            default:
                GFX_ASSERTMSG(0, "An unsupported parameter type `%u' was supplied", i);
                break;
            }
    }

    GfxResult createKernel(Program const &program, Kernel &kernel)
    {
        char const *kernel_type = nullptr;
        GfxResult result = kGfxResult_NoError;
        GFX_ASSERT(kernel.num_threads_ != nullptr);
        GFX_ASSERT(kernel.cs_bytecode_ == nullptr && kernel.cs_reflection_ == nullptr);
        GFX_ASSERT(kernel.as_bytecode_ == nullptr && kernel.as_reflection_ == nullptr);
        GFX_ASSERT(kernel.ms_bytecode_ == nullptr && kernel.ms_reflection_ == nullptr);
        GFX_ASSERT(kernel.vs_bytecode_ == nullptr && kernel.vs_reflection_ == nullptr);
        GFX_ASSERT(kernel.gs_bytecode_ == nullptr && kernel.gs_reflection_ == nullptr);
        GFX_ASSERT(kernel.ps_bytecode_ == nullptr && kernel.ps_reflection_ == nullptr);
        GFX_ASSERT(kernel.lib_bytecode_ == nullptr && kernel.lib_reflection_ == nullptr);
        GFX_ASSERT(kernel.root_signature_ == nullptr);
        GFX_ASSERT(kernel.pipeline_state_ == nullptr);
        GFX_ASSERT(kernel.parameters_ == nullptr);
        if(kernel.isMesh())
        {
            kernel_type = "Mesh";
            compileShader(program, kernel, kShaderType_AS, kernel.as_bytecode_, kernel.as_reflection_);
            compileShader(program, kernel, kShaderType_MS, kernel.ms_bytecode_, kernel.ms_reflection_);
            compileShader(program, kernel, kShaderType_PS, kernel.ps_bytecode_, kernel.ps_reflection_);
            createRootSignature(kernel);
            result = createMeshPipelineState(kernel, kernel.draw_state_);
            if(kernel.as_reflection_ != nullptr)
                kernel.as_reflection_->GetThreadGroupSize(&kernel.num_threads_[0], &kernel.num_threads_[1], &kernel.num_threads_[2]);
            else if(kernel.ms_reflection_ != nullptr)
                kernel.ms_reflection_->GetThreadGroupSize(&kernel.num_threads_[0], &kernel.num_threads_[1], &kernel.num_threads_[2]);
            else
                for(uint32_t i = 0; i < 3; ++i) kernel.num_threads_[i] = 1;
        }
        else if(kernel.isCompute())
        {
            kernel_type = "Compute";
            compileShader(program, kernel, kShaderType_CS, kernel.cs_bytecode_, kernel.cs_reflection_);
            createRootSignature(kernel);
            createComputePipelineState(kernel);
            if(kernel.cs_reflection_ != nullptr)
                kernel.cs_reflection_->GetThreadGroupSize(&kernel.num_threads_[0], &kernel.num_threads_[1], &kernel.num_threads_[2]);
            else
                for(uint32_t i = 0; i < 3; ++i) kernel.num_threads_[i] = 1;
        }
        else if(kernel.isGraphics())
        {
            kernel_type = "Graphics";
            compileShader(program, kernel, kShaderType_VS, kernel.vs_bytecode_, kernel.vs_reflection_);
            compileShader(program, kernel, kShaderType_GS, kernel.gs_bytecode_, kernel.gs_reflection_);
            compileShader(program, kernel, kShaderType_PS, kernel.ps_bytecode_, kernel.ps_reflection_);
            createRootSignature(kernel);
            result = createGraphicsPipelineState(kernel, kernel.draw_state_);
        }
        else if(kernel.isRaytracing())
        {
            kernel_type = "Raytracing";
            compileShader(program, kernel, kShaderType_LIB, kernel.lib_bytecode_, kernel.lib_reflection_);
            createRootSignature(kernel);
            createRaytracingPipelineState(kernel);
        }
        else
            return GFX_SET_ERROR(kGfxResult_InternalError, "Cannot create unsupported kernel type");
        std::vector<char> buffer((program.file_name_ ? program.file_name_.size() : program.file_path_.size() + kernel.entry_point_.size() + strlen(kernel_type)) + 21);
        if(kernel.root_signature_ != nullptr)
        {
            GFX_SNPRINTF(buffer.data(), buffer.size(), "%s::%s_%sRootSignature", program.file_name_ ? program.file_name_.c_str() : program.file_path_.c_str(), kernel.entry_point_.c_str(), kernel_type);
            SetDebugName(kernel.root_signature_, buffer.data());
        }
        if(kernel.pipeline_state_ != nullptr)
        {
            GFX_SNPRINTF(buffer.data(), buffer.size(), "%s::%s_%sPipelineSignature", program.file_name_ ? program.file_name_.c_str() : program.file_path_.c_str(), kernel.entry_point_.c_str(), kernel_type);
            SetDebugName(kernel.pipeline_state_, buffer.data());
        }
        return result;
    }

    void reloadKernel(Kernel &kernel)
    {
        if(!program_handles_.has_handle(kernel.program_.handle)) return;
        Program const &program = programs_[kernel.program_];
        kernel.descriptor_heap_id_ = 0;
        if(kernel.cs_bytecode_ != nullptr) { kernel.cs_bytecode_->Release(); kernel.cs_bytecode_ = nullptr; }
        if(kernel.as_bytecode_ != nullptr) { kernel.as_bytecode_->Release(); kernel.as_bytecode_ = nullptr; }
        if(kernel.ms_bytecode_ != nullptr) { kernel.ms_bytecode_->Release(); kernel.ms_bytecode_ = nullptr; }
        if(kernel.vs_bytecode_ != nullptr) { kernel.vs_bytecode_->Release(); kernel.vs_bytecode_ = nullptr; }
        if(kernel.gs_bytecode_ != nullptr) { kernel.gs_bytecode_->Release(); kernel.gs_bytecode_ = nullptr; }
        if(kernel.ps_bytecode_ != nullptr) { kernel.ps_bytecode_->Release(); kernel.ps_bytecode_ = nullptr; }
        if(kernel.lib_bytecode_ != nullptr) { kernel.lib_bytecode_->Release(); kernel.lib_bytecode_ = nullptr; }
        if(kernel.cs_reflection_ != nullptr) { kernel.cs_reflection_->Release(); kernel.cs_reflection_ = nullptr; }
        if(kernel.as_reflection_ != nullptr) { kernel.as_reflection_->Release(); kernel.as_reflection_ = nullptr; }
        if(kernel.ms_reflection_ != nullptr) { kernel.ms_reflection_->Release(); kernel.ms_reflection_ = nullptr; }
        if(kernel.vs_reflection_ != nullptr) { kernel.vs_reflection_->Release(); kernel.vs_reflection_ = nullptr; }
        if(kernel.gs_reflection_ != nullptr) { kernel.gs_reflection_->Release(); kernel.gs_reflection_ = nullptr; }
        if(kernel.ps_reflection_ != nullptr) { kernel.ps_reflection_->Release(); kernel.ps_reflection_ = nullptr; }
        if(kernel.lib_reflection_ != nullptr) { kernel.lib_reflection_->Release(); kernel.lib_reflection_ = nullptr; }
        if(kernel.root_signature_ != nullptr) { collect(kernel.root_signature_); kernel.root_signature_ = nullptr; }
        if(kernel.pipeline_state_ != nullptr) { collect(kernel.pipeline_state_); kernel.pipeline_state_ = nullptr; }
        if(kernel.state_object_ != nullptr) { collect(kernel.state_object_); kernel.state_object_ = nullptr; }
        for(uint32_t i = 0; i < kernel.parameter_count_; ++i)
        {
            freeDescriptor(kernel.parameters_[i].descriptor_slot_);
            for(uint32_t j = 0; j < kernel.parameters_[i].variable_count_; ++j)
                kernel.parameters_[i].variables_[j].~Variable();
            gfxFree(kernel.parameters_[i].variables_);
            kernel.parameters_[i].~Parameter();
        }
        gfxFree(kernel.parameters_);
        kernel.parameters_ = nullptr;
        kernel.parameter_count_ = 0;
        kernel.vertex_stride_ = 0;
        createKernel(program, kernel);
    }

    struct Shader
    {
        IDxcBlob *shader_bytecode_ = nullptr;
        ID3D12ShaderReflection *shader_reflection_ = nullptr;
    };
    std::map<uint64_t, Shader> shaders_;

    template<typename REFLECTION_TYPE>
    void compileShader(Program const &program, Kernel const &kernel, ShaderType shader_type, IDxcBlob *&shader_bytecode, REFLECTION_TYPE *&reflection)
    {
        DxcBuffer shader_source = {};
        IDxcBlobEncoding *dxc_source = nullptr;
        std::vector<char> shader_file(program.file_path_.size() + program.file_name_.size() + strlen(shader_extensions_[shader_type]) + 2);
        std::vector<WCHAR> wshader_file;
        GFX_ASSERT(shader_type < kShaderType_Count);

        if(program.file_name_)
        {
            GFX_SNPRINTF(shader_file.data(), shader_file.size(), "%s/%s%s", program.file_path_.c_str(), program.file_name_.c_str(), shader_extensions_[shader_type]);
            wshader_file.resize(mbstowcs(nullptr, shader_file.data(), 0) + 1);
            mbstowcs(wshader_file.data(), shader_file.data(), shader_file.size());
            // Check file existence before LoadFile call. LoadFile spams hlsl::Exception messages if file not found.
            if(GetFileAttributesW(wshader_file.data()) == INVALID_FILE_ATTRIBUTES) return;
            dxc_utils_->LoadFile(wshader_file.data(), nullptr, &dxc_source);
            if(!dxc_source) return; // failed to load source file
            shader_source.Ptr = dxc_source->GetBufferPointer();
            shader_source.Size = dxc_source->GetBufferSize();
        }
        else
        {
            GFX_SNPRINTF(shader_file.data(), shader_file.size(), "%s%s", program.file_path_.c_str(), shader_extensions_[shader_type]);
            wshader_file.resize(mbstowcs(nullptr, shader_file.data(), 0) + 1);
            mbstowcs(wshader_file.data(), shader_file.data(), shader_file.size());
            switch(shader_type)
            {
            case kShaderType_CS:
                shader_source.Ptr = program.cs_.c_str();
                break;
            case kShaderType_AS:
                shader_source.Ptr = program.as_.c_str();
                break;
            case kShaderType_MS:
                shader_source.Ptr = program.ms_.c_str();
                break;
            case kShaderType_VS:
                shader_source.Ptr = program.vs_.c_str();
                break;
            case kShaderType_GS:
                shader_source.Ptr = program.gs_.c_str();
                break;
            case kShaderType_PS:
                shader_source.Ptr = program.ps_.c_str();
                break;
            case kShaderType_LIB:
                shader_source.Ptr = program.lib_.c_str();
                break;
            default:
                GFX_ASSERTMSG(0, "An unsupported shader type `%u' was supplied", (uint32_t)shader_type);
                return;
            }
            shader_source.Size = strlen((char const *)shader_source.Ptr);
            if(shader_source.Size == 0) return; // no source found for this shader
        }

        char shader_profiles[][16] =
        {
            "cs_",
            "as_",
            "ms_",
            "vs_",
            "gs_",
            "ps_",
            "lib_"
        };
        uint32_t const shader_profile_count = (uint32_t)std::size(shader_profiles);
        static_assert(shader_profile_count == kShaderType_Count, "An invalid number of shader profiles was supplied");
        for(uint32_t i = 0; i < shader_profile_count; ++i) strcpy(shader_profiles[i] + strlen(shader_profiles[i]), program.shader_model_.c_str());

        std::vector<WCHAR> wentry_point(mbstowcs(nullptr, kernel.entry_point_.c_str(), 0) + 1);
        mbstowcs(wentry_point.data(), kernel.entry_point_.c_str(), wentry_point.size());
        std::vector<WCHAR> wshader_profile(mbstowcs(nullptr, shader_profiles[shader_type], 0) + 1);
        mbstowcs(wshader_profile.data(), shader_profiles[shader_type], wshader_profile.size());

        std::vector<LPCWSTR> shader_args;
        shader_args.push_back(wshader_file.data());
        if(dxr_device_ != nullptr)
            shader_args.push_back(L"-enable-16bit-types");
        shader_args.push_back(L"-I"); shader_args.push_back(L".");
        shader_args.push_back(L"-T"); shader_args.push_back(wshader_profile.data());
        shader_args.push_back(L"-HV 2021");
        shader_args.push_back(L"-Wno-parameter-usage");
        shader_args.push_back(L"-Wno-uninitialized");
        shader_args.push_back(L"-Wno-conditional-uninitialized");
        shader_args.push_back(L"-Wno-sometimes-uninitialized");
        shader_args.push_back(L"-fdiagnostics-format=msvc");
        if(experimental_shaders_)
        {
            shader_args.push_back(DXC_ARG_SKIP_VALIDATION);
            shader_args.push_back(L"-select-validator internal");
        }

        std::vector<std::wstring> exports;
        if(shader_type == kShaderType_LIB)
        {
            if(!kernel.exports_.empty())
            {
                size_t max_export_length = 0;
                for(size_t i = 0; i < kernel.exports_.size(); ++i)
                    max_export_length = GFX_MAX(max_export_length, strlen(kernel.exports_[i].c_str()));
                max_export_length += 1;
                std::vector<char> lib_export(max_export_length);
                std::vector<WCHAR> wexport(max_export_length);
                for(size_t i = 0; i < kernel.exports_.size(); ++i)
                {
                    GFX_SNPRINTF(lib_export.data(), lib_export.size(), "%s", kernel.exports_[i].c_str());
                    mbstowcs(wexport.data(), lib_export.data(), max_export_length);
                    exports.push_back(wexport.data());
                }
                for(size_t i = 0; i < exports.size(); ++i)
                {
                    shader_args.push_back(L"-exports");
                    shader_args.push_back(exports[i].c_str());
                }
            }
            shader_args.push_back(L"-auto-binding-space 0");
        }
        else
        {
            shader_args.push_back(L"-E"); shader_args.push_back(wentry_point.data());
        }

        if(debug_shaders_)
        {
            shader_args.push_back(DXC_ARG_DEBUG);
            shader_args.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
            shader_args.push_back(DXC_ARG_DEBUG_NAME_FOR_SOURCE);
        }

        std::vector<std::wstring> user_defines;
        if(!kernel.defines_.empty())
        {
            size_t max_define_length = 0;
            for(size_t i = 0; i < kernel.defines_.size(); ++i)
                max_define_length = GFX_MAX(max_define_length, strlen(kernel.defines_[i].c_str()));
            max_define_length += 3; // `//' + null terminator: https://github.com/gboisse/gfx/issues/41
            std::vector<WCHAR> wdefine(max_define_length << 1);
            std::vector<char> define(max_define_length);
            for(size_t i = 0; i < kernel.defines_.size(); ++i)
            {
                GFX_SNPRINTF(define.data(), max_define_length, "%s//", kernel.defines_[i].c_str());
                mbstowcs(wdefine.data(), define.data(), max_define_length);
                user_defines.push_back(wdefine.data());
            }
            for(size_t i = 0; i < user_defines.size(); ++i)
            {
                shader_args.push_back(L"-D");
                shader_args.push_back(user_defines[i].c_str());
            }
        }

        std::vector<std::wstring> include_paths;
        if(!program.include_paths_.empty())
        {
            include_paths.reserve(program.include_paths_.size());
            for(size_t i = 0; i < program.include_paths_.size(); ++i)
            {
                size_t const path_length = std::strlen(program.include_paths_[i]);
                std::wstring include_path(path_length, L' ');
                include_path.resize(std::mbstowcs(include_path.data(), program.include_paths_[i], path_length));

                include_paths.push_back(include_path);
            }
            for(size_t i = 0; i < include_paths.size(); ++i)
            {
                shader_args.push_back(L"-I");
                shader_args.push_back(include_paths[i].c_str());
            }
        }

        uint64_t shader_key = 0;
        std::string shader_key_bytecode;
        std::string shader_key_reflection;
        if constexpr(std::is_same<ID3D12ShaderReflection, REFLECTION_TYPE>::value)
        {
            IDxcResult *dxc_preprocess = nullptr;
            shader_args.push_back(L"-P");   // run DXC as preprocessor
            dxc_compiler_->Compile(&shader_source, shader_args.data(), (uint32_t)shader_args.size(), dxc_include_handler_, IID_PPV_ARGS(&dxc_preprocess));
            if(dxc_preprocess != nullptr)
            {
                IDxcBlob *dxc_hlsl = nullptr;
                dxc_preprocess->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(&dxc_hlsl), nullptr);
                if(dxc_hlsl != nullptr)
                {
                    std::string const hlsl((char *)dxc_hlsl->GetBufferPointer(), dxc_hlsl->GetBufferSize());
                    HashCombine(shader_key, Hash(kernel.entry_point_.c_str()));
                    for(size_t i = 1; i < shader_args.size(); ++i)
                    {
                        char buffer[64] = {};
                        std::size_t ret = wcstombs(buffer, shader_args[i], sizeof(buffer));
                        buffer[GFX_MIN(ret, std::size(buffer) - 1)] = '\0'; // make sure we have a null terminator since Hash function expects it
                        HashCombine(shader_key, Hash(buffer));
                    }
                    for(String const &define : kernel.defines_)
                        HashCombine(shader_key, Hash(define.c_str()));
                    HashCombine(shader_key, Hash(hlsl.c_str()));
                    HashCombine(shader_key, shader_type);
                    dxc_hlsl->Release();
                }
                dxc_preprocess->Release();
            }
            shader_args.pop_back();
            if(shader_key != 0)
            {
                std::map<uint64_t, Shader>::const_iterator const it = shaders_.find(shader_key);
                if(it != shaders_.end())
                {
                    shader_bytecode = (*it).second.shader_bytecode_;
                    reflection = (*it).second.shader_reflection_;
                    GFX_ASSERT(shader_bytecode != nullptr && reflection != nullptr);
                    if(dxc_source) dxc_source->Release();
                    return; // done
                }
                if(cache_shaders_)
                {
                    std::string shader_key_file = "./shader_cache/";
                    static bool created_shader_cache_directory;
                    if(!created_shader_cache_directory)
                    {
                        int32_t const result = _mkdir(shader_key_file.c_str());
                        if(result < 0 && errno != EEXIST)
                            GFX_PRINT_ERROR(kGfxResult_InternalError, "Failed to create `%s' directory; cannot write shader cache", shader_key_file.c_str());
                        created_shader_cache_directory = true;  // do not attempt creating the shader cache directory again
                    }
                    shader_key_file += std::to_string(shader_key);
                    shader_key_bytecode = shader_key_file + ".bytecode";
                    shader_key_reflection = shader_key_file + ".reflection";
                    std::vector<WCHAR> wshader_key_bytecode(shader_key_bytecode.size() + 1);
                    std::vector<WCHAR> wshader_key_reflection(shader_key_reflection.size() + 1);
                    memset(wshader_key_bytecode.data(), 0, wshader_key_bytecode.size() * sizeof(WCHAR));
                    memset(wshader_key_reflection.data(), 0, wshader_key_reflection.size() * sizeof(WCHAR));
                    mbstowcs(wshader_key_bytecode.data(), shader_key_bytecode.data(), shader_key_bytecode.size());
                    mbstowcs(wshader_key_reflection.data(), shader_key_reflection.data(), shader_key_reflection.size());
                    IDxcBlobEncoding *bytecode_blob = nullptr, *reflection_blob = nullptr;
                    dxc_utils_->LoadFile(wshader_key_bytecode.data(), nullptr, &bytecode_blob);
                    dxc_utils_->LoadFile(wshader_key_reflection.data(), nullptr, &reflection_blob);
                    if(bytecode_blob != nullptr && reflection_blob != nullptr)
                    {
                        DxcBuffer reflection_data = {};
                        reflection_data.Size = reflection_blob->GetBufferSize();
                        reflection_data.Ptr = reflection_blob->GetBufferPointer();
                        dxc_utils_->CreateReflection(&reflection_data, IID_PPV_ARGS(&reflection));
                        if(reflection != nullptr)
                        {
                            Shader &shader = shaders_[shader_key];
                            shader.shader_bytecode_ = bytecode_blob;
                            shader.shader_reflection_ = reflection;
                            shader_bytecode = bytecode_blob;
                            reflection_blob->Release();
                            return; // done
                        }
                    }
                    if(bytecode_blob) bytecode_blob->Release();
                    if(reflection_blob) reflection_blob->Release();
                }
            }
        }

        IDxcResult *dxc_result = nullptr;
        dxc_compiler_->Compile(&shader_source, shader_args.data(), (uint32_t)shader_args.size(), dxc_include_handler_, IID_PPV_ARGS(&dxc_result));
        if(dxc_source) dxc_source->Release();
        if(!dxc_result) return; // should never happen?

        HRESULT result_code = E_FAIL;
        dxc_result->GetStatus(&result_code);
        IDxcBlobEncoding *dxc_error = nullptr;
        dxc_result->GetErrorBuffer(&dxc_error);
        if(FAILED(result_code))
        {
            bool const has_errors = (dxc_error != nullptr && dxc_error->GetBufferPointer() != nullptr);
            GFX_PRINTLN("Error: Failed to compile `%s' for entry point `%s'%s%s", shader_file.data(), kernel.entry_point_.c_str(), has_errors ? ":\r\n" : "", has_errors ? (char const *)dxc_error->GetBufferPointer() : "");
            if(dxc_error) dxc_error->Release(); dxc_result->Release();
            return; // failed to compile
        }
        if(dxc_error != nullptr)
        {
            bool const has_warnings = (dxc_error->GetBufferPointer() != nullptr);
            if(has_warnings)
                GFX_PRINTLN("Compiled `%s' for entry point `%s' with warning(s):\r\n%s", shader_file.data(), kernel.entry_point_.c_str(), (char const *)dxc_error->GetBufferPointer());
            dxc_error->Release();
        }

        IDxcBlob *dxc_pdb = nullptr;
        IDxcBlob *dxc_bytecode = nullptr;
        IDxcBlob *dxc_reflection = nullptr;
        IDxcBlobUtf16 *dxc_pdb_name = nullptr;
        dxc_result->GetResult(&dxc_bytecode);
        if(debug_shaders_)
            dxc_result->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&dxc_pdb), &dxc_pdb_name);
        dxc_result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&dxc_reflection), nullptr);
        dxc_result->Release();
        if(!dxc_bytecode || !dxc_reflection)
        {
            if(dxc_pdb) dxc_pdb->Release();
            if(dxc_pdb_name) dxc_pdb_name->Release();
            if(dxc_bytecode) dxc_bytecode->Release();
            if(dxc_reflection) dxc_reflection->Release();
            return; // should never happen?
        }
        if(dxc_pdb != nullptr && dxc_pdb_name != nullptr)
        {
            static bool created_shader_pdb_directory;
            std::string_view const shader_pdb_directory = "./shader_pdb";
            std::wstring const wpdb_name(dxc_pdb_name->GetStringPointer(), dxc_pdb_name->GetStringLength());
            std::vector<char> pdb_name(wcstombs(nullptr, wpdb_name.c_str(), 0) + 1);
            wcstombs(pdb_name.data(), wpdb_name.c_str(), pdb_name.size());
            shader_file.resize(shader_pdb_directory.size() + pdb_name.size() + 1);
            GFX_SNPRINTF(shader_file.data(), shader_file.size(), "%s/%s", shader_pdb_directory.data(), pdb_name.data());
            if(!created_shader_pdb_directory)
            {
                int32_t const result = _mkdir(shader_pdb_directory.data());
                if(result < 0 && errno != EEXIST)
                    GFX_PRINT_ERROR(kGfxResult_InternalError, "Failed to create `%s' directory; cannot write shader PDBs", shader_pdb_directory.data());
                created_shader_pdb_directory = true;    // do not attempt creating the shader PDB directory again
            }
            FILE *fd = fopen(shader_file.data(), "wb");
            if(fd != nullptr)
            {
                fwrite(dxc_pdb->GetBufferPointer(), dxc_pdb->GetBufferSize(), 1, fd);
                fclose(fd); // write out PDB for shader debugging
            }
        }

        DxcBuffer reflection_data = {};
        reflection_data.Size = dxc_reflection->GetBufferSize();
        reflection_data.Ptr = dxc_reflection->GetBufferPointer();
        dxc_utils_->CreateReflection(&reflection_data, IID_PPV_ARGS(&reflection));

        if(shader_key != 0 && dxc_bytecode != nullptr && reflection != nullptr)
        {
            if constexpr(std::is_same<ID3D12ShaderReflection, REFLECTION_TYPE>::value)
            {
                GFX_ASSERT(shaders_.find(shader_key) == shaders_.end());
                Shader &shader = shaders_[shader_key];
                shader.shader_bytecode_ = dxc_bytecode;
                shader.shader_reflection_ = reflection;
                if(cache_shaders_)
                {
                    FILE *fd = fopen(shader_key_bytecode.c_str(), "wb");
                    if(fd)
                    {
                        fwrite(dxc_bytecode->GetBufferPointer(), dxc_bytecode->GetBufferSize(), 1, fd);
                        fclose(fd); // write out bytecode for shader caching
                    }
                    fd = fopen(shader_key_reflection.c_str(), "wb");
                    if(fd)
                    {
                        fwrite(dxc_reflection->GetBufferPointer(), dxc_reflection->GetBufferSize(), 1, fd);
                        fclose(fd); // write out reflection for shader caching
                    }
                }
            }
        }
        if(reflection) shader_bytecode = dxc_bytecode;
        if(!reflection) dxc_bytecode->Release();
        if(dxc_pdb_name) dxc_pdb_name->Release();
        if(dxc_pdb) dxc_pdb->Release();
        dxc_reflection->Release();
    }

    GfxResult createResource(D3D12MA::ALLOCATION_DESC const &allocation_desc, D3D12_RESOURCE_DESC const &resource_desc,
        D3D12_RESOURCE_STATES initial_resource_state, float const *clear_value, D3D12MA::Allocation **allocation, REFIID riid_resource, void **ppv_resource)
    {
        HRESULT result;
        D3D12_CLEAR_VALUE
        clear_color        = {};
        clear_color.Format = resource_desc.Format;
        if(clear_value != nullptr)
        {
            if(IsDepthStencilFormat(resource_desc.Format))
            {
                clear_color.DepthStencil.Depth   = clear_value[0];
                clear_color.DepthStencil.Stencil = (UINT8)clear_value[1];
            }
            else
            {
                clear_color.Color[0] = clear_value[0];
                clear_color.Color[1] = clear_value[1];
                clear_color.Color[2] = clear_value[2];
                clear_color.Color[3] = clear_value[3];
            }
        }
        result = mem_allocator_->CreateResource(&allocation_desc, &resource_desc, initial_resource_state,
            (resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 || (resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ? &clear_color : nullptr,
            allocation, riid_resource, ppv_resource);
        if(!SUCCEEDED(result))
        {
            *allocation = nullptr;  // D3D12MemoryAllocator leaves a dangling pointer to the allocation object upon failure
            return GFX_SET_ERROR(kGfxResult_OutOfMemory, "Unable to allocate memory to create resource");
        }
        return kGfxResult_NoError;
    }

    enum TransitionType
    {
        kTransitionType_Implicit = 0,
        kTransitionType_Explicit,

        kTransitionType_Count
    };

    bool transitionResource(Buffer &buffer, D3D12_RESOURCE_STATES resource_state, TransitionType transition_type = kTransitionType_Explicit)
    {
        GFX_ASSERT(buffer.data_ == nullptr); if(buffer.data_ != nullptr) return false;
        if((*buffer.resource_state_ & resource_state) == resource_state)
        {
            if(*buffer.resource_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                bool already_has_uav_barrier = false;
                for(std::vector<D3D12_RESOURCE_BARRIER>::const_iterator it = resource_barriers_.begin(); it != resource_barriers_.end(); ++it)
                    if(((*it).Type == D3D12_RESOURCE_BARRIER_TYPE_UAV        && (*it).UAV.pResource        == buffer.resource_) ||
                       ((*it).Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && (*it).Transition.pResource == buffer.resource_))
                    {
                        already_has_uav_barrier = true;
                        break;  // already has a UAV barrier
                    }
                if(!already_has_uav_barrier)
                {
                    D3D12_RESOURCE_BARRIER resource_barrier = {};
                    resource_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    resource_barrier.UAV.pResource = buffer.resource_;
                    resource_barriers_.push_back(resource_barrier);
                }
                return !already_has_uav_barrier;
            }
            return false;
        }
        if(transition_type == kTransitionType_Implicit && !*buffer.transitioned_)
        {
            if(*buffer.resource_state_ == D3D12_RESOURCE_STATE_COMMON)
            {
                *buffer.resource_state_ = resource_state;
                return false;
            }
            else if(((*buffer.resource_state_ & D3D12_RESOURCE_STATE_GENERIC_READ) == *buffer.resource_state_) && ((resource_state & D3D12_RESOURCE_STATE_GENERIC_READ) == resource_state))
            {
                *buffer.resource_state_ |= resource_state;
                return false;
            }
        }
        for(std::vector<D3D12_RESOURCE_BARRIER>::const_iterator it = resource_barriers_.begin(); it != resource_barriers_.end(); ++it)
            if(((*it).Type == D3D12_RESOURCE_BARRIER_TYPE_UAV        && (*it).UAV.pResource        == buffer.resource_) ||
                ((*it).Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && (*it).Transition.pResource == buffer.resource_))
            {
                submitPipelineBarriers();
                break;  // break barrier batches to avoid debug layer warnings
            }
        if(*buffer.resource_state_ == D3D12_RESOURCE_STATE_INDEX_BUFFER &&  // unbind if active index buffer to prevent debug layer errors
            buffer_handles_.has_handle(bound_index_buffer_.handle) && buffers_[bound_index_buffer_].resource_ == buffer.resource_)
        {
            command_list_->IASetIndexBuffer(nullptr);
            force_install_index_buffer_ = true;
        }
        if(*buffer.resource_state_ == D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER &&    // unbind if active vertex buffer to prevent debug layer errors
            buffer_handles_.has_handle(bound_vertex_buffer_.handle) && buffers_[bound_vertex_buffer_].resource_ == buffer.resource_)
        {
            command_list_->IASetVertexBuffers(0, 1, nullptr);
            force_install_vertex_buffer_ = true;
        }
        D3D12_RESOURCE_BARRIER resource_barrier = {};
        resource_barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resource_barrier.Transition.pResource   = buffer.resource_;
        resource_barrier.Transition.StateBefore = *buffer.resource_state_;
        resource_barrier.Transition.StateAfter  = resource_state;
        resource_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        resource_barriers_.push_back(resource_barrier);
        *buffer.resource_state_ = resource_state;
        *buffer.transitioned_ = true;
        return true;
    }

    bool transitionResource(Texture &texture, D3D12_RESOURCE_STATES resource_state, TransitionType transition_type = kTransitionType_Explicit)
    {
        if((texture.resource_state_ & resource_state) == resource_state)
        {
            if(texture.resource_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                bool already_has_uav_barrier = false;
                for(std::vector<D3D12_RESOURCE_BARRIER>::const_iterator it = resource_barriers_.begin(); it != resource_barriers_.end(); ++it)
                    if(((*it).Type == D3D12_RESOURCE_BARRIER_TYPE_UAV        && (*it).UAV.pResource        == texture.resource_) ||
                       ((*it).Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && (*it).Transition.pResource == texture.resource_))
                    {
                        already_has_uav_barrier = true;
                        break;  // already has a UAV barrier
                    }
                if(!already_has_uav_barrier)
                {
                    D3D12_RESOURCE_BARRIER resource_barrier = {};
                    resource_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    resource_barrier.UAV.pResource = texture.resource_;
                    resource_barriers_.push_back(resource_barrier);
                }
                return !already_has_uav_barrier;
            }
            return false;
        }
        if(transition_type == kTransitionType_Implicit && !texture.transitioned_)
        {
            if(texture.resource_state_ == D3D12_RESOURCE_STATE_COMMON && (((resource_state & (D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE)) == resource_state)))
            {
                texture.resource_state_ = resource_state;
                return false;
            }
            else if(((texture.resource_state_ & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) == texture.resource_state_) && (resource_state & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) != 0)
            {
                texture.resource_state_ |= resource_state;
                return false;
            }
        }
        for(std::vector<D3D12_RESOURCE_BARRIER>::const_iterator it = resource_barriers_.begin(); it != resource_barriers_.end(); ++it)
            if(((*it).Type == D3D12_RESOURCE_BARRIER_TYPE_UAV        && (*it).UAV.pResource        == texture.resource_) ||
               ((*it).Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && (*it).Transition.pResource == texture.resource_))
            {
                submitPipelineBarriers();
                break;  // break barrier batches to avoid debug layer warnings
            }
        D3D12_RESOURCE_BARRIER resource_barrier = {};
        resource_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resource_barrier.Transition.pResource = texture.resource_;
        resource_barrier.Transition.StateBefore = texture.resource_state_;
        resource_barrier.Transition.StateAfter = resource_state;
        resource_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        resource_barriers_.push_back(resource_barrier);
        texture.resource_state_ = resource_state;
        texture.transitioned_ = true;
        return true;
    }

    void submitPipelineBarriers()
    {
        if(resource_barriers_.empty()) return;  // no pending barriers
        command_list_->ResourceBarrier((uint32_t)resource_barriers_.size(),
                                       resource_barriers_.data());
        resource_barriers_.clear();
    }

    GfxResult acquireSwapChainBuffers()
    {
        for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
        {
            char buffer[25];
            GFX_SNPRINTF(buffer, sizeof(buffer), "gfx_BackBuffer%u", i);
            if(!SUCCEEDED(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]))))
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to acquire back buffer");
            SetDebugName(back_buffers_[i], buffer);
        }

        return kGfxResult_NoError;
    }

    GfxResult createBackBuffers()
    {
        for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
        {
            char buffer[25];
            GFX_SNPRINTF(buffer, sizeof(buffer), "gfx_BackBuffer%u", i);

            D3D12_RESOURCE_STATES const resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_RESOURCE_DESC
            resource_desc                  = {};
            resource_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resource_desc.Width            = window_width_;
            resource_desc.Height           = window_height_;
            resource_desc.DepthOrArraySize = 1;
            resource_desc.MipLevels        = 1;
            resource_desc.Format           = back_buffer_format_;
            resource_desc.SampleDesc.Count = 1;
            resource_desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12MA::ALLOCATION_DESC allocation_desc = {};
            allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            float const clear_value[] = { 0.0f, 0.0f, 0.0f, 1.0f };

            GFX_TRY(createResource(allocation_desc, resource_desc, resource_state, clear_value,
                &back_buffer_allocations_[i], IID_PPV_ARGS(&back_buffers_[i])));

            SetDebugName(back_buffers_[i], buffer);
        }

        return kGfxResult_NoError;
    }

    GfxResult createBackBufferRTVs()
    {
        for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
        {
            back_buffer_rtvs_[i] = allocateRTVDescriptor();
            if(back_buffer_rtvs_[i] == 0xFFFFFFFFu)
                return GFX_SET_ERROR(kGfxResult_InternalError, "Unable to allocate RTV descriptors");
            GFX_ASSERT(back_buffers_[i] != nullptr);
            device_->CreateRenderTargetView(back_buffers_[i], nullptr, rtv_descriptors_.getCPUHandle(back_buffer_rtvs_[i]));
        }

        return kGfxResult_NoError;
    }

    GfxResult sync()
    {
        for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
        {
            command_queue_->Signal(fences_[i], ++fence_values_[i]);
            if(fences_[i]->GetCompletedValue() != fence_values_[i])
            {
                fences_[i]->SetEventOnCompletion(fence_values_[i], fence_event_);
                WaitForSingleObject(fence_event_, INFINITE);    // wait for GPU to complete
            }
        }
        return forceGarbageCollection();
    }

    GfxResult resizeCallback(uint32_t window_width, uint32_t window_height)
    {
        if(!IsWindow(window_)) return kGfxResult_NoError;   // can't resize past window tear down
        GFX_ASSERT(swap_chain_ != nullptr);
        for(uint32_t i = 0; i < textures_.size(); ++i)
        {
            Texture &texture = textures_.data()[i];
            if(!((texture.flags_ & Texture::kFlag_AutoResize) != 0))
                continue;   // no need to auto-resize
            ID3D12Resource *resource = nullptr;
            D3D12MA::Allocation *allocation = nullptr;
            D3D12_RESOURCE_DESC
            resource_desc        = texture.resource_->GetDesc();
            resource_desc.Width  = window_width;
            resource_desc.Height = window_height;
            D3D12MA::ALLOCATION_DESC allocation_desc = {};
            allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            if(createResource(allocation_desc, resource_desc, D3D12_RESOURCE_STATE_COMMON, texture.clear_value_, &allocation, IID_PPV_ARGS(&resource)) != kGfxResult_NoError)
            {
                GFX_PRINT_ERROR(kGfxResult_OutOfMemory, "Unable to auto-resize texture object(s) to %ux%u", window_width, window_height);
                break;  // out of memory
            }
            collect(texture);   // release previous texture
            texture.resource_ = resource;
            texture.allocation_ = allocation;
            texture.Object::flags_ &= ~Object::kFlag_Named;
            texture.resource_state_ = D3D12_RESOURCE_STATE_COMMON;
            texture.initial_resource_state_ = D3D12_RESOURCE_STATE_COMMON;
            texture.transitioned_ = false;
            for(uint32_t j = 0; j < ARRAYSIZE(texture.dsv_descriptor_slots_); ++j)
            {
                texture.dsv_descriptor_slots_[j].resize(resource_desc.DepthOrArraySize);
                for(size_t k = 0; k < texture.dsv_descriptor_slots_[j].size(); ++k)
                    texture.dsv_descriptor_slots_[j][k] = 0xFFFFFFFFu;
            }
            for(uint32_t j = 0; j < ARRAYSIZE(texture.rtv_descriptor_slots_); ++j)
            {
                texture.rtv_descriptor_slots_[j].resize(resource_desc.DepthOrArraySize);
                for(size_t k = 0; k < texture.rtv_descriptor_slots_[j].size(); ++k)
                    texture.rtv_descriptor_slots_[j][k] = 0xFFFFFFFFu;
            }
        }
        for(uint32_t i = 0; i < max_frames_in_flight_; ++i)
        {
            collect(back_buffers_[i]);
            back_buffers_[i] = nullptr;
            freeRTVDescriptor(back_buffer_rtvs_[i]);
        }
        sync(); // make sure the GPU is done with the previous swap chain before resizing
        window_width_  = window_width;
        window_height_ = window_height;
        HRESULT const hr = swap_chain_->ResizeBuffers(max_frames_in_flight_, window_width, window_height, back_buffer_format_, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        if(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        {
            GFX_TRY(handleDeviceLost());
        }
        else if(FAILED(hr))
        {
            return GFX_SET_ERROR(kGfxResult_InternalError, "Error detected during resizeBuffers: %s", hr);
        }
        fence_index_ = swap_chain_->GetCurrentBackBufferIndex();
        GFX_TRY(acquireSwapChainBuffers());
        GFX_TRY(createBackBufferRTVs());
        return kGfxResult_NoError;
    }

    GfxResult handleDeviceLost()
    {
        HRESULT const reason = device_->GetDeviceRemovedReason();
        LPSTR error_text = nullptr;
        DWORD const result = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, nullptr, reason,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&error_text), 0, nullptr);
        if (command_list_ != nullptr)
        {
            command_list_->Release();
            command_list_ = nullptr;
        }
        if(result > 0)
        {
            GFX_PRINT_ERROR(kGfxResult_DeviceError, "Device error \"%s\", DXGI_ERROR code: 0x%X", error_text, reason);
            LocalFree(error_text);
        }
        else
            GFX_PRINT_ERROR(kGfxResult_DeviceError, "Device error, DXGI_ERROR code: 0x%X", reason);
        return GFX_SET_ERROR(kGfxResult_DeviceError, "Device failed");
    }
};

char const *GfxInternal::shader_extensions_[] =
{
    ".comp",
    ".task",
    ".mesh",
    ".vert",
    ".geom",
    ".frag",
    ".rt"
};

uint32_t const GfxInternal::kNumThreads_Invalid[] =
{
    1,
    1,
    1
};

GfxArray<GfxInternal::DrawState> GfxInternal::draw_states_;
GfxHandles                       GfxInternal::draw_state_handles_("draw state");

GfxContext gfxCreateContext(HWND window, GfxCreateContextFlags flags, IDXGIAdapter *adapter)
{
    GfxResult result;
    GfxContext context = {};
    if(!window) return context; // invalid param
    GfxInternal *gfx = new GfxInternal(context);
    if(!gfx) return context;    // out of memory
    result = gfx->initialize(window, flags, adapter, context);
    if(result != kGfxResult_NoError)
    {
        delete gfx;
        context = {};
        GFX_PRINT_ERROR(result, "Failed to create graphics context");
    }
    return context;
}

GfxContext gfxCreateContext(uint32_t width, uint32_t height, GfxCreateContextFlags flags, IDXGIAdapter *adapter)
{
    GfxResult result;
    GfxContext context = {};
    GfxInternal *gfx = new GfxInternal(context);
    if(!gfx) return context;    // out of memory
    result = gfx->initialize(width, height, flags, adapter, context);
    if(result != kGfxResult_NoError)
    {
        delete gfx;
        context = {};
        GFX_PRINT_ERROR(result, "Failed to create graphics context");
    }
    return context;
}

GfxResult gfxDestroyContext(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_NoError; // nothing to destroy
    if(!gfx->isInterop() && gfx->isValid()) GFX_TRY(gfx->finish()); delete gfx;
    return kGfxResult_NoError;
}

bool gfxContextIsValid(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return false;
    return gfx->isValid();
}

uint32_t gfxGetBackBufferWidth(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return 0;  // invalid context
    return gfx->getBackBufferWidth();
}

uint32_t gfxGetBackBufferHeight(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return 0;  // invalid context
    return gfx->getBackBufferHeight();
}

uint32_t gfxGetBackBufferIndex(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return 0;  // invalid context
    return gfx->getBackBufferIndex();
}

uint32_t gfxGetBackBufferCount(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return 0;  // invalid context
    return gfx->getBackBufferCount();
}

DXGI_FORMAT gfxGetBackBufferFormat(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return DXGI_FORMAT_UNKNOWN;  // invalid context
    return gfx->getBackBufferFormat();
}

DXGI_COLOR_SPACE_TYPE gfxGetBackBufferColorSpace(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return DXGI_COLOR_SPACE_RESERVED;  // invalid context
    return gfx->getBackBufferColorSpace();
}

GfxDisplayDesc gfxGetDisplayDescription(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return GfxDisplayDesc{};  // invalid context
    return gfx->getDisplayDescription();
}

bool gfxIsRaytracingSupported(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return false;  // invalid context
    return gfx->isRaytracingSupported();
}

bool gfxIsMeshShaderSupported(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return false;  // invalid context
    return gfx->isMeshShaderSupported();
}

bool gfxIsInteropContext(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return false;  // invalid context
    return gfx->isInterop();
}

GfxBuffer gfxCreateBuffer(GfxContext context, uint64_t size, void const *data, GfxCpuAccess cpu_access)
{
    GfxBuffer const buffer = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return buffer; // invalid context
    return gfx->createBuffer(size, data, cpu_access);
}

GfxBuffer gfxCreateBufferRange(GfxContext context, GfxBuffer buffer, uint64_t byte_offset, uint64_t size)
{
    GfxBuffer const buffer_range = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return buffer_range;   // invalid context
    return gfx->createBufferRange(buffer, byte_offset, size);
}

GfxResult gfxDestroyBuffer(GfxContext context, GfxBuffer buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->destroyBuffer(buffer);
}

void *gfxBufferGetData(GfxContext context, GfxBuffer buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return nullptr;    // invalid context
    return gfx->getBufferData(buffer);
}

GfxTexture gfxCreateTexture2D(GfxContext context, DXGI_FORMAT format, float const *clear_value)
{
    GfxTexture const texture = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return texture;    // invalid context
    return gfx->createTexture2D(format, clear_value);
}

GfxTexture gfxCreateTexture2D(GfxContext context, uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t mip_levels, float const *clear_value)
{
    GfxTexture const texture = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return texture;    // invalid context
    return gfx->createTexture2D(width, height, format, mip_levels, clear_value);
}

GfxTexture gfxCreateTexture2DArray(GfxContext context, uint32_t width, uint32_t height, uint32_t slice_count, DXGI_FORMAT format, uint32_t mip_levels, float const *clear_value)
{
    GfxTexture const texture = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return texture;    // invalid context
    return gfx->createTexture2DArray(width, height, slice_count, format, mip_levels, clear_value);
}

GfxTexture gfxCreateTexture3D(GfxContext context, uint32_t width, uint32_t height, uint32_t depth, DXGI_FORMAT format, uint32_t mip_levels, float const *clear_value)
{
    GfxTexture const texture = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return texture;    // invalid context
    return gfx->createTexture3D(width, height, depth, format, mip_levels, clear_value);
}

GfxTexture gfxCreateTextureCube(GfxContext context, uint32_t size, DXGI_FORMAT format, uint32_t mip_levels, float const *clear_value)
{
    GfxTexture const texture = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return texture;    // invalid context
    return gfx->createTextureCube(size, format, mip_levels, clear_value);
}

GfxResult gfxDestroyTexture(GfxContext context, GfxTexture texture)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->destroyTexture(texture);
}

uint32_t gfxCalculateMipCount(uint32_t width, uint32_t height, uint32_t depth)
{
    uint32_t mip_count = 0;
    uint32_t mip_size  = GFX_MAX(width, GFX_MAX(height, depth));
    while(mip_size >= 1) { mip_size >>= 1; ++mip_count; }
    return mip_count;
}

GfxSamplerState gfxCreateSamplerState(GfxContext context, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address_u, D3D12_TEXTURE_ADDRESS_MODE address_v, D3D12_TEXTURE_ADDRESS_MODE address_w, float mip_lod_bias, float min_lod, float max_lod)
{
    GfxSamplerState const sampler_state = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return sampler_state;  // invalid context
    return gfx->createSamplerState(filter, address_u, address_v, address_w, mip_lod_bias, min_lod, max_lod);
}

GfxSamplerState gfxCreateSamplerState(GfxContext context, D3D12_FILTER filter, D3D12_COMPARISON_FUNC comparison_func, D3D12_TEXTURE_ADDRESS_MODE address_u, D3D12_TEXTURE_ADDRESS_MODE address_v, D3D12_TEXTURE_ADDRESS_MODE address_w, float mip_lod_bias, float min_lod, float max_lod)
{
    GfxSamplerState const sampler_state = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return sampler_state;  // invalid context
    return gfx->createSamplerState(filter, comparison_func, address_u, address_v, address_w, mip_lod_bias, min_lod, max_lod);
}

GfxResult gfxDestroySamplerState(GfxContext context, GfxSamplerState sampler_state)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->destroySamplerState(sampler_state);
}

GfxAccelerationStructure gfxCreateAccelerationStructure(GfxContext context)
{
    GfxAccelerationStructure const acceleration_structure = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return acceleration_structure; // invalid context
    return gfx->createAccelerationStructure();
}

GfxResult gfxDestroyAccelerationStructure(GfxContext context, GfxAccelerationStructure acceleration_structure)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->destroyAccelerationStructure(acceleration_structure);
}

GfxResult gfxAccelerationStructureUpdate(GfxContext context, GfxAccelerationStructure acceleration_structure)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->updateAccelerationStructure(acceleration_structure);
}

uint64_t gfxAccelerationStructureGetDataSize(GfxContext context, GfxAccelerationStructure acceleration_structure)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return 0;  // invalid context
    return gfx->getAccelerationStructureDataSize(acceleration_structure);
}

GfxRaytracingPrimitive gfxCreateRaytracingPrimitive(GfxContext context, GfxAccelerationStructure acceleration_structure)
{
    GfxRaytracingPrimitive const raytracing_primitive = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return raytracing_primitive;   // invalid context
    return gfx->createRaytracingPrimitive(acceleration_structure);
}

GfxRaytracingPrimitive gfxCreateRaytracingPrimitiveInstance(GfxContext context, GfxRaytracingPrimitive raytracing_primitive)
{
    GfxRaytracingPrimitive const cloned_raytracing_primitive = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return cloned_raytracing_primitive;    // invalid context
    return gfx->createRaytracingPrimitiveInstance(raytracing_primitive);
}

GfxRaytracingPrimitive gfxCreateRaytracingPrimitiveProcedural(GfxContext context, GfxAccelerationStructure acceleration_structure)
{
    GfxRaytracingPrimitive const raytracing_primitive = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return raytracing_primitive;   // invalid context
    return gfx->createRaytracingPrimitiveProcedural(acceleration_structure);
}

GfxResult gfxDestroyRaytracingPrimitive(GfxContext context, GfxRaytracingPrimitive raytracing_primitive)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->destroyRaytracingPrimitive(raytracing_primitive);
}

GfxResult gfxRaytracingPrimitiveBuild(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, GfxBuffer vertex_buffer, uint32_t vertex_stride, GfxBuildRaytracingPrimitiveFlags build_flags)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->buildRaytracingPrimitive(raytracing_primitive, vertex_buffer, vertex_stride, build_flags);
}

GfxResult gfxRaytracingPrimitiveBuild(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, GfxBuffer index_buffer, GfxBuffer vertex_buffer, uint32_t vertex_stride, GfxBuildRaytracingPrimitiveFlags build_flags)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->buildRaytracingPrimitive(raytracing_primitive, index_buffer, vertex_buffer, vertex_stride, build_flags);
}

GfxResult gfxRaytracingPrimitiveBuildProcedural(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, GfxBuffer aabb_buffer, uint32_t aabb_stride, GfxBuildRaytracingPrimitiveFlags build_flags)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->buildRaytracingPrimitiveProcedural(raytracing_primitive, aabb_buffer, aabb_stride, build_flags);
}

GfxResult gfxRaytracingPrimitiveSetTransform(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, float const *row_major_4x4_transform)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setRaytracingPrimitiveTransform(raytracing_primitive, row_major_4x4_transform);
}

GfxResult gfxRaytracingPrimitiveSetInstanceID(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, uint32_t instance_id)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setRaytracingPrimitiveInstanceID(raytracing_primitive, instance_id);
}

GfxResult gfxRaytracingPrimitiveSetInstanceMask(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, uint8_t instance_mask)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setRaytracingPrimitiveInstanceMask(raytracing_primitive, instance_mask);
}

GfxResult gfxRaytracingPrimitiveSetInstanceContributionToHitGroupIndex(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, uint32_t instance_contribution_to_hit_group_index)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setRaytracingPrimitiveInstanceContributionToHitGroupIndex(raytracing_primitive, instance_contribution_to_hit_group_index);
}

uint64_t gfxRaytracingPrimitiveGetDataSize(GfxContext context, GfxRaytracingPrimitive raytracing_primitive)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return 0;  // invalid context
    return gfx->getRaytracingPrimitiveDataSize(raytracing_primitive);
}

GfxResult gfxRaytracingPrimitiveUpdate(GfxContext context, GfxRaytracingPrimitive raytracing_primitive)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->updateRaytracingPrimitive(raytracing_primitive);
}

GfxResult gfxRaytracingPrimitiveUpdate(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, GfxBuffer vertex_buffer, uint32_t vertex_stride)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->updateRaytracingPrimitive(raytracing_primitive, vertex_buffer, vertex_stride);
}

GfxResult gfxRaytracingPrimitiveUpdate(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, GfxBuffer index_buffer, GfxBuffer vertex_buffer, uint32_t vertex_stride)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->updateRaytracingPrimitive(raytracing_primitive, index_buffer, vertex_buffer, vertex_stride);
}

GfxResult gfxRaytracingPrimitiveUpdateProcedural(GfxContext context, GfxRaytracingPrimitive raytracing_primitive, GfxBuffer aabb_buffer, uint32_t aabb_stride)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->updateRaytracingPrimitiveProcedural(raytracing_primitive, aabb_buffer, aabb_stride);
}

GfxDrawState::GfxDrawState()
{
    GfxInternal::DispenseDrawState(*this);
}

GfxDrawState::GfxDrawState(GfxDrawState const &other)
    : handle(other.handle)
{
    GfxInternal::RetainDrawState(*this);
}

GfxDrawState &GfxDrawState::operator =(GfxDrawState const &other)
{
    GfxInternal::ReleaseDrawState(*this);
    handle = other.handle;
    GfxInternal::RetainDrawState(*this);
    return *this;
}

GfxDrawState::~GfxDrawState()
{
    GfxInternal::ReleaseDrawState(*this);
}

GfxResult gfxDrawStateSetColorTarget(GfxDrawState draw_state, uint32_t target_index, DXGI_FORMAT texture_format)
{
    return GfxInternal::SetDrawStateColorTarget(draw_state, target_index, texture_format);
}

GfxResult gfxDrawStateSetDepthStencilTarget(GfxDrawState draw_state, DXGI_FORMAT texture_format)
{
    return GfxInternal::SetDrawStateDepthStencilTarget(draw_state, texture_format);
}

GfxResult gfxDrawStateSetCullMode(GfxDrawState draw_state, D3D12_CULL_MODE cull_mode)
{
    return GfxInternal::SetDrawStateCullMode(draw_state, cull_mode);
}

GfxResult gfxDrawStateSetFillMode(GfxDrawState draw_state, D3D12_FILL_MODE fill_mode)
{
    return GfxInternal::SetDrawStateFillMode(draw_state, fill_mode);
}

GfxResult gfxDrawStateSetDepthFunction(GfxDrawState draw_state, D3D12_COMPARISON_FUNC depth_function)
{
    return GfxInternal::SetDrawStateDepthFunction(draw_state, depth_function);
}

GfxResult gfxDrawStateSetDepthWriteMask(GfxDrawState draw_state, D3D12_DEPTH_WRITE_MASK depth_write_mask)
{
    return GfxInternal::SetDrawStateDepthWriteMask(draw_state, depth_write_mask);
}

GfxResult gfxDrawStateSetPrimitiveTopologyType(GfxDrawState draw_state, D3D12_PRIMITIVE_TOPOLOGY_TYPE primitive_topology_type)
{
    return GfxInternal::SetDrawStatePrimitiveTopologyType(draw_state, primitive_topology_type);
}

GfxResult gfxDrawStateSetBlendMode(GfxDrawState draw_state, D3D12_BLEND src_blend, D3D12_BLEND dst_blend, D3D12_BLEND_OP blend_op, D3D12_BLEND src_blend_alpha, D3D12_BLEND dst_blend_alpha, D3D12_BLEND_OP blend_op_alpha)
{
    return GfxInternal::SetDrawStateBlendMode(draw_state, src_blend, dst_blend, blend_op, src_blend_alpha, dst_blend_alpha, blend_op_alpha);
}

GfxResult gfxDrawStateEnableAlphaBlending(GfxDrawState draw_state)
{
    return gfxDrawStateSetBlendMode(draw_state, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD);
}

GfxProgram gfxCreateProgram(GfxContext context, char const *file_name, char const *file_path, char const *shader_model, char const **include_paths, uint32_t include_path_count)
{
    GfxProgram const program = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return program;    // invalid context
    return gfx->createProgram(file_name, file_path, shader_model, include_paths, include_path_count);
}

GfxProgram gfxCreateProgram(GfxContext context, GfxProgramDesc program_desc, char const *name, char const *shader_model, char const **include_paths, uint32_t include_path_count)
{
    GfxProgram const program = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return program;    // invalid context
    return gfx->createProgram(program_desc, name, shader_model, include_paths, include_path_count);
}

GfxResult gfxDestroyProgram(GfxContext context, GfxProgram program)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->destroyProgram(program);
}

GfxResult gfxProgramSetBuffer(GfxContext context, GfxProgram program, char const *parameter_name, GfxBuffer buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setProgramBuffer(program, parameter_name, buffer);
}

GfxResult gfxProgramSetBuffers(GfxContext context, GfxProgram program, char const *parameter_name, GfxBuffer const *buffers, uint32_t buffer_count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setProgramBuffers(program, parameter_name, buffers, buffer_count);
}

GfxResult gfxProgramSetTexture(GfxContext context, GfxProgram program, char const *parameter_name, GfxTexture texture, uint32_t mip_level)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setProgramTexture(program, parameter_name, texture, mip_level);
}

GfxResult gfxProgramSetTextures(GfxContext context, GfxProgram program, char const *parameter_name, GfxTexture const *textures, uint32_t texture_count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setProgramTextures(program, parameter_name, textures, nullptr, texture_count);
}

GfxResult gfxProgramSetTextures(GfxContext context, GfxProgram program, char const *parameter_name, GfxTexture const *textures, uint32_t const *mip_levels, uint32_t texture_count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setProgramTextures(program, parameter_name, textures, mip_levels, texture_count);
}

GfxResult gfxProgramSetSamplerState(GfxContext context, GfxProgram program, char const *parameter_name, GfxSamplerState sampler_state)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setProgramSamplerState(program, parameter_name, sampler_state);
}

GfxResult gfxProgramSetAccelerationStructure(GfxContext context, GfxProgram program, char const *parameter_name, GfxAccelerationStructure acceleration_structure)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setProgramAccelerationStructure(program, parameter_name, acceleration_structure);
}

GfxResult gfxProgramSetConstants(GfxContext context, GfxProgram program, char const *parameter_name, void const *data, uint32_t data_size)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setProgramConstants(program, parameter_name, data, data_size);
}

GfxKernel gfxCreateMeshKernel(GfxContext context, GfxProgram program, char const *entry_point, char const **defines, uint32_t define_count)
{
    GfxDrawState const default_draw_state = {};
    return gfxCreateMeshKernel(context, program, default_draw_state, entry_point, defines, define_count);
}

GfxKernel gfxCreateMeshKernel(GfxContext context, GfxProgram program, GfxDrawState draw_state, char const *entry_point, char const **defines, uint32_t define_count)
{
    GfxKernel const mesh_kernel = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return mesh_kernel;    // invalid context
    return gfx->createMeshKernel(program, draw_state, entry_point, defines, define_count);
}

GfxKernel gfxCreateComputeKernel(GfxContext context, GfxProgram program, char const *entry_point, char const **defines, uint32_t define_count)
{
    GfxKernel const compute_kernel = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return compute_kernel; // invalid context
    return gfx->createComputeKernel(program, entry_point, defines, define_count);
}

GfxKernel gfxCreateGraphicsKernel(GfxContext context, GfxProgram program, char const *entry_point, char const **defines, uint32_t define_count)
{
    GfxDrawState const default_draw_state = {};
    return gfxCreateGraphicsKernel(context, program, default_draw_state, entry_point, defines, define_count);
}

GfxKernel gfxCreateGraphicsKernel(GfxContext context, GfxProgram program, GfxDrawState draw_state, char const *entry_point, char const **defines, uint32_t define_count)
{
    GfxKernel const graphics_kernel = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return graphics_kernel;    // invalid context
    return gfx->createGraphicsKernel(program, draw_state, entry_point, defines, define_count);
}

GfxKernel gfxCreateRaytracingKernel(GfxContext context, GfxProgram program,
    GfxLocalRootSignatureAssociation const *local_root_signature_associations, uint32_t local_root_signature_association_count,
    char const **exports, uint32_t export_count,
    char const **subobjects, uint32_t subobject_count,
    char const **defines, uint32_t define_count)
{
    GfxKernel const raytracing_kernel = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return raytracing_kernel;    // invalid context
    return gfx->createRaytracingKernel(program, local_root_signature_associations, local_root_signature_association_count, exports, export_count, subobjects, subobject_count, defines, define_count);
}

GfxResult gfxDestroyKernel(GfxContext context, GfxKernel kernel)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->destroyKernel(kernel);
}

uint32_t const *gfxKernelGetNumThreads(GfxContext context, GfxKernel kernel)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return GfxInternal::kNumThreads_Invalid;
    return gfx->getKernelNumThreads(kernel);
}

GfxResult gfxKernelReloadAll(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->reloadAllKernels();
}

GfxSbt gfxCreateSbt(GfxContext context, GfxKernel const *kernels, uint32_t kernel_count, uint32_t entry_count[kGfxShaderGroupType_Count])
{
    GfxSbt const sbt = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return sbt;   // invalid context
    return gfx->createSbt(kernels, kernel_count, entry_count);
}

GfxResult gfxDestroySbt(GfxContext context, GfxSbt sbt)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->destroySbt(sbt);
}

GfxResult gfxSbtSetShaderGroup(GfxContext context, GfxSbt sbt, GfxShaderGroupType shader_group_type, uint32_t index, char const *group_name)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->sbtSetShaderGroup(sbt, shader_group_type, index, group_name);
}

GfxResult gfxSbtSetConstants(GfxContext context, GfxSbt sbt, GfxShaderGroupType shader_group_type, uint32_t index, char const *parameter_name, void const *data, uint32_t data_size)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->sbtSetConstants(sbt, shader_group_type, index, parameter_name, data, data_size);
}

GfxResult gfxSbtSetTexture(GfxContext context, GfxSbt sbt, GfxShaderGroupType shader_group_type, uint32_t index, char const *parameter_name, GfxTexture texture, uint32_t mip_level)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->sbtSetTexture(sbt, shader_group_type, index, parameter_name, texture, mip_level);
}

GfxResult gfxSbtGetGpuVirtualAddressRangeAndStride(GfxContext context,
    GfxSbt sbt,
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE *ray_generation_shader_record,
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE *miss_shader_table,
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE *hit_group_table,
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE *callable_shader_table)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->sbtGetGpuVirtualAddressRangeAndStride(sbt,
        ray_generation_shader_record,
        miss_shader_table,
        hit_group_table,
        callable_shader_table);
}

GfxResult gfxCommandCopyBuffer(GfxContext context, GfxBuffer dst, GfxBuffer src)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeCopyBuffer(dst, src);
}

GfxResult gfxCommandCopyBuffer(GfxContext context, GfxBuffer dst, uint64_t dst_offset, GfxBuffer src, uint64_t src_offset, uint64_t size)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeCopyBuffer(dst, dst_offset, src, src_offset, size);
}

GfxResult gfxCommandClearBuffer(GfxContext context, GfxBuffer buffer, uint32_t clear_value)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeClearBuffer(buffer, clear_value);
}

GfxResult gfxCommandClearBackBuffer(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeClearBackBuffer();
}

GfxResult gfxCommandClearTexture(GfxContext context, GfxTexture texture)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeClearTexture(texture);
}

GfxResult gfxCommandCopyTexture(GfxContext context, GfxTexture dst, GfxTexture src)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeCopyTexture(dst, src);
}

GfxResult gfxCommandClearImage(GfxContext context, GfxTexture texture, uint32_t mip_level, uint32_t slice)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeClearImage(texture, mip_level, slice);
}

GfxResult gfxCommandCopyTextureToBackBuffer(GfxContext context, GfxTexture texture)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeCopyTextureToBackBuffer(texture);
}

GfxResult gfxCommandCopyBufferToTexture(GfxContext context, GfxTexture dst, GfxBuffer src)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeCopyBufferToTexture(dst, src);
}

GfxResult gfxCommandCopyBufferToCubeFace(GfxContext context, GfxTexture dst, GfxBuffer src, uint32_t face)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeCopyBufferToCubeFace(dst, src, face);
}

GfxResult gfxCommandCopyTextureToBuffer(GfxContext context, GfxBuffer dst, GfxTexture src)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeCopyTextureToBuffer(dst, src);
}

GfxResult gfxCommandGenerateMips(GfxContext context, GfxTexture texture)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeGenerateMips(texture);
}

GfxResult gfxCommandBindColorTarget(GfxContext context, uint32_t target_index, GfxTexture target_texture, uint32_t mip_level, uint32_t slice)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeBindColorTarget(target_index, target_texture, mip_level, slice);
}

GfxResult gfxCommandBindDepthStencilTarget(GfxContext context, GfxTexture target_texture, uint32_t mip_level, uint32_t slice)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeBindDepthStencilTarget(target_texture, mip_level, slice);
}

GfxResult gfxCommandBindKernel(GfxContext context, GfxKernel kernel)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeBindKernel(kernel);
}

GfxResult gfxCommandBindIndexBuffer(GfxContext context, GfxBuffer index_buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeBindIndexBuffer(index_buffer);
}

GfxResult gfxCommandBindVertexBuffer(GfxContext context, GfxBuffer vertex_buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeBindVertexBuffer(vertex_buffer);
}

GfxResult gfxCommandSetViewport(GfxContext context, float x, float y, float width, float height)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeSetViewport(x, y, width, height);
}

GfxResult gfxCommandSetScissorRect(GfxContext context, int32_t x, int32_t y, int32_t width, int32_t height)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeSetScissorRect(x, y, width, height);
}

GfxResult gfxCommandDraw(GfxContext context, uint32_t vertex_count, uint32_t instance_count, uint32_t base_vertex, uint32_t base_instance)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeDraw(vertex_count, instance_count, base_vertex, base_instance);
}

GfxResult gfxCommandDrawIndexed(GfxContext context, uint32_t index_count, uint32_t instance_count, uint32_t first_index, uint32_t base_vertex, uint32_t base_instance)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeDrawIndexed(index_count, instance_count, first_index, base_vertex, base_instance);
}

GfxResult gfxCommandMultiDrawIndirect(GfxContext context, GfxBuffer args_buffer, uint32_t args_count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeMultiDrawIndirect(args_buffer, args_count);
}

GfxResult gfxCommandMultiDrawIndexedIndirect(GfxContext context, GfxBuffer args_buffer, uint32_t args_count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeMultiDrawIndexedIndirect(args_buffer, args_count);
}

GfxResult gfxCommandDispatch(GfxContext context, uint32_t num_groups_x, uint32_t num_groups_y, uint32_t num_groups_z)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeDispatch(num_groups_x, num_groups_y, num_groups_z);
}

GfxResult gfxCommandDispatchIndirect(GfxContext context, GfxBuffer args_buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeDispatchIndirect(args_buffer);
}

GfxResult gfxCommandMultiDispatchIndirect(GfxContext context, GfxBuffer args_buffer, uint32_t args_count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeMultiDispatchIndirect(args_buffer, args_count);
}

GfxResult gfxCommandDispatchRays(GfxContext context, GfxSbt sbt, uint32_t width, uint32_t height, uint32_t depth)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeDispatchRays(sbt, width, height, depth);
}

GfxResult gfxCommandDispatchRaysIndirect(GfxContext context, GfxSbt sbt, GfxBuffer args_buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeDispatchRaysIndirect(sbt, args_buffer);
}

GfxResult gfxCommandDrawMesh(GfxContext context, uint32_t num_groups_x, uint32_t num_groups_y, uint32_t num_groups_z)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeDrawMesh(num_groups_x, num_groups_y, num_groups_z);
}

GfxResult gfxCommandDrawMeshIndirect(GfxContext context, GfxBuffer args_buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeDrawMeshIndirect(args_buffer);
}

GfxTimestampQuery gfxCreateTimestampQuery(GfxContext context)
{
    GfxTimestampQuery const timestamp_query = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return timestamp_query;    // invalid context
    return gfx->createTimestampQuery();
}

GfxResult gfxDestroyTimestampQuery(GfxContext context, GfxTimestampQuery timestamp_query)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->destroyTimestampQuery(timestamp_query);
}

float gfxTimestampQueryGetDuration(GfxContext context, GfxTimestampQuery timestamp_query)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return 0.0f;   // invalid context
    return gfx->getTimestampQueryDuration(timestamp_query);
}

GfxResult gfxCommandBeginTimestampQuery(GfxContext context, GfxTimestampQuery timestamp_query)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeBeginTimestampQuery(timestamp_query);
}

GfxResult gfxCommandEndTimestampQuery(GfxContext context, GfxTimestampQuery timestamp_query)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeEndTimestampQuery(timestamp_query);
}

GfxResult gfxCommandBeginEvent(GfxContext context, char const *format, ...)
{
    va_list args;
    GfxResult result;
    va_start(args, format);
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) result = kGfxResult_InvalidParameter;
        else result = gfx->encodeBeginEvent(0, format, args);
    va_end(args);   // release variadic arguments
    return result;
}

GfxResult gfxCommandBeginEvent(GfxContext context, uint64_t color, char const *format, ...)
{
    va_list args;
    GfxResult result;
    va_start(args, format);
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) result = kGfxResult_InvalidParameter;
        else result = gfx->encodeBeginEvent(color, format, args);
    va_end(args);   // release variadic arguments
    return result;
}

GfxResult gfxCommandEndEvent(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeEndEvent();
}

GfxResult gfxCommandScanMin(GfxContext context, GfxDataType data_type, GfxBuffer dst, GfxBuffer src, GfxBuffer const *count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeScan(GfxInternal::kOpType_Min, data_type, dst, src, count);
}

GfxResult gfxCommandScanMax(GfxContext context, GfxDataType data_type, GfxBuffer dst, GfxBuffer src, GfxBuffer const *count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeScan(GfxInternal::kOpType_Max, data_type, dst, src, count);
}

GfxResult gfxCommandScanSum(GfxContext context, GfxDataType data_type, GfxBuffer dst, GfxBuffer src, GfxBuffer const *count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeScan(GfxInternal::kOpType_Sum, data_type, dst, src, count);
}

GfxResult gfxCommandReduceMin(GfxContext context, GfxDataType data_type, GfxBuffer dst, GfxBuffer src, GfxBuffer const *count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeReduce(GfxInternal::kOpType_Min, data_type, dst, src, count);
}

GfxResult gfxCommandReduceMax(GfxContext context, GfxDataType data_type, GfxBuffer dst, GfxBuffer src, GfxBuffer const *count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeReduce(GfxInternal::kOpType_Max, data_type, dst, src, count);
}

GfxResult gfxCommandReduceSum(GfxContext context, GfxDataType data_type, GfxBuffer dst, GfxBuffer src, GfxBuffer const *count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeReduce(GfxInternal::kOpType_Sum, data_type, dst, src, count);
}

GfxResult gfxCommandSortRadix(GfxContext context, GfxBuffer keys_dst, GfxBuffer keys_src, GfxBuffer const *values_dst, GfxBuffer const *values_src, GfxBuffer const *count)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->encodeRadixSort(keys_dst, keys_src, values_dst, values_src, count);
}

GfxResult gfxFrame(GfxContext context, bool vsync)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->frame(vsync);
}

GfxResult gfxFinish(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->finish();
}

GfxContext gfxCreateContext(ID3D12Device *device, uint32_t max_frames_in_flight)
{
    GfxResult result;
    GfxContext context = {};
    if(!device) return context; // invalid param
    GfxInternal *gfx = new GfxInternal(context);
    if(!gfx) return context;    // out of memory
    result = gfx->initialize(device, max_frames_in_flight, context);
    if(result != kGfxResult_NoError)
    {
        delete gfx;
        context = {};
        GFX_PRINT_ERROR(result, "Failed to create graphics context");
    }
    return context;
}

ID3D12Device *gfxGetDevice(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return nullptr;    // invalid context
    return gfx->getDevice();
}

ID3D12CommandQueue *gfxGetCommandQueue(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return nullptr;    // invalid context
    return gfx->getCommandQueue();
}

ID3D12GraphicsCommandList *gfxGetCommandList(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return nullptr;    // invalid context
    return gfx->getCommandList();
}

GfxResult gfxSetCommandList(GfxContext context, ID3D12GraphicsCommandList *command_list)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->setCommandList(command_list);
}

GfxResult gfxResetCommandListState(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->resetCommandListState();
}

GfxBuffer gfxCreateBuffer(GfxContext context, ID3D12Resource *resource, D3D12_RESOURCE_STATES resource_state)
{
    GfxBuffer const buffer = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return buffer; // invalid context
    return gfx->createBuffer(resource, resource_state);
}

GfxTexture gfxCreateTexture(GfxContext context, ID3D12Resource *resource, D3D12_RESOURCE_STATES resource_state)
{
    GfxTexture const texture = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return texture;    // invalid context
    return gfx->createTexture(resource, resource_state);
}

GfxAccelerationStructure gfxCreateAccelerationStructure(GfxContext context, ID3D12Resource *resource, uint64_t byte_offset)
{
    GfxAccelerationStructure const acceleration_structure = {};
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return acceleration_structure; // invalid context
    return gfx->createAccelerationStructure(resource, byte_offset);
}

ID3D12Resource *gfxBufferGetResource(GfxContext context, GfxBuffer buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return nullptr;    // invalid context
    return gfx->getBufferResource(buffer);
}

ID3D12Resource *gfxTextureGetResource(GfxContext context, GfxTexture texture)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return nullptr;    // invalid context
    return gfx->getTextureResource(texture);
}

ID3D12Resource *gfxAccelerationStructureGetResource(GfxContext context, GfxAccelerationStructure acceleration_structure)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return nullptr;    // invalid context
    return gfx->getAccelerationStructureResource(acceleration_structure);
}

D3D12_RESOURCE_STATES gfxBufferGetResourceState(GfxContext context, GfxBuffer buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return D3D12_RESOURCE_STATE_COMMON;    // invalid context
    return gfx->getBufferResourceState(buffer);
}

D3D12_RESOURCE_STATES gfxTextureGetResourceState(GfxContext context, GfxTexture texture)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return D3D12_RESOURCE_STATE_COMMON;    // invalid context
    return gfx->getTextureResourceState(texture);
}

HANDLE gfxBufferCreateSharedHandle(GfxContext context, GfxBuffer buffer)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return nullptr;    // invalid context
    return gfx->createBufferSharedHandle(buffer);
}

GfxResult gfxExecute(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->execute();
}

GfxResult gfxResetCommandList(GfxContext context)
{
    GfxInternal *gfx = GfxInternal::GetGfx(context);
    if(!gfx) return kGfxResult_InvalidParameter;
    return gfx->resetCommandList();
}
