/*
 * Copyright 2016-2017 Józef Kucia for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* Hack for MinGW-w64 headers.
 *
 * We want to use WIDL C inline wrappers because some methods
 * in D3D12 interfaces return aggregate objects. Unfortunately,
 * WIDL C inline wrappers are broken when used with MinGW-w64
 * headers because FORCEINLINE expands to extern inline
 * which leads to the "multiple storage classes in declaration
 * specifiers" compiler error.
 */
#ifdef __MINGW32__
# include <_mingw.h>
# ifdef __MINGW64_VERSION_MAJOR
#  undef __forceinline
#  define __forceinline __inline__ __attribute__((__always_inline__,__gnu_inline__))
# endif

# define _HRESULT_DEFINED
typedef int HRESULT;
#endif

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <time.h>

#define COBJMACROS
#define INITGUID
#include "vkd3d_test.h"
#include "vkd3d_windows.h"
#define WIDL_C_INLINE_WRAPPERS
#include "d3d12.h"

#ifdef _WIN32
# include "dxgi1_4.h"
#else
# include <pthread.h>
# include "vkd3d_utils.h"
#endif

static void set_rect(RECT *rect, int left, int top, int right, int bottom)
{
    rect->left = left;
    rect->right = right;
    rect->top = top;
    rect->bottom = bottom;
}

static void set_viewport(D3D12_VIEWPORT *vp, float x, float y,
        float width, float height, float min_depth, float max_depth)
{
    vp->TopLeftX = x;
    vp->TopLeftY = y;
    vp->Width = width;
    vp->Height = height;
    vp->MinDepth = min_depth;
    vp->MaxDepth = max_depth;
}

struct vec2
{
    float x, y;
};

struct vec4
{
    float x, y, z, w;
};

struct uvec4
{
    unsigned int x, y, z, w;
};

struct ivec4
{
    int x, y, z, w;
};

static bool compare_float(float f, float g, unsigned int ulps)
{
    int x, y;
    union
    {
        float f;
        int i;
    } u;

    u.f = f;
    x = u.i;
    u.f = g;
    y = u.i;

    if (x < 0)
        x = INT_MIN - x;
    if (y < 0)
        y = INT_MIN - y;

    if (abs(x - y) > ulps)
        return false;

    return true;
}

static bool compare_vec4(const struct vec4 *v1, const struct vec4 *v2, unsigned int ulps)
{
    return compare_float(v1->x, v2->x, ulps)
            && compare_float(v1->y, v2->y, ulps)
            && compare_float(v1->z, v2->z, ulps)
            && compare_float(v1->w, v2->w, ulps);
}

static bool compare_uvec4(const struct uvec4* v1, const struct uvec4 *v2)
{
    return v1->x == v2->x && v1->y == v2->y && v1->z == v2->z && v1->w == v2->w;
}

static bool compare_uint16(uint16_t a, uint16_t b, unsigned int max_diff)
{
    return abs(a - b) <= max_diff;
}

static bool compare_color(DWORD c1, DWORD c2, BYTE max_diff)
{
    if (abs((c1 & 0xff) - (c2 & 0xff)) > max_diff)
        return false;
    c1 >>= 8; c2 >>= 8;
    if (abs((c1 & 0xff) - (c2 & 0xff)) > max_diff)
        return false;
    c1 >>= 8; c2 >>= 8;
    if (abs((c1 & 0xff) - (c2 & 0xff)) > max_diff)
        return false;
    c1 >>= 8; c2 >>= 8;
    if (abs((c1 & 0xff) - (c2 & 0xff)) > max_diff)
        return false;
    return true;
}

static ULONG get_refcount(void *iface)
{
    IUnknown *unk = iface;
    IUnknown_AddRef(unk);
    return IUnknown_Release(unk);
}

#define check_interface(a, b, c) check_interface_(__LINE__, (IUnknown *)a, b, c)
static void check_interface_(unsigned int line, IUnknown *iface, REFIID riid, bool supported)
{
    HRESULT hr, expected_hr;
    IUnknown *unk;

    expected_hr = supported ? S_OK : E_NOINTERFACE;

    hr = IUnknown_QueryInterface(iface, riid, (void **)&unk);
    ok_(line)(hr == expected_hr, "Got hr %#x, expected %#x.\n", hr, expected_hr);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
}

static HRESULT create_root_signature(ID3D12Device *device, const D3D12_ROOT_SIGNATURE_DESC *desc,
        ID3D12RootSignature **root_signature)
{
    ID3DBlob *blob;
    HRESULT hr;

    if (FAILED(hr = D3D12SerializeRootSignature(desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, NULL)))
        return hr;

    hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)root_signature);
    ID3D10Blob_Release(blob);
    return hr;
}

static D3D12_SHADER_BYTECODE shader_bytecode(const DWORD *code, size_t size)
{
    D3D12_SHADER_BYTECODE shader_bytecode = { code, size };
    return shader_bytecode;
}

static void transition_sub_resource_state(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
        unsigned int sub_resource_idx, D3D12_RESOURCE_STATES state_before, D3D12_RESOURCE_STATES state_after)
{
    D3D12_RESOURCE_BARRIER barrier;

    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = sub_resource_idx;
    barrier.Transition.StateBefore = state_before;
    barrier.Transition.StateAfter = state_after;

    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

static void transition_resource_state(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
        D3D12_RESOURCE_STATES state_before, D3D12_RESOURCE_STATES state_after)
{
    transition_sub_resource_state(list, resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            state_before, state_after);
}

static void uav_barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource)
{
    D3D12_RESOURCE_BARRIER barrier;

    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;

    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

static void exec_command_list(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list)
{
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)list};
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
}

#define reset_command_list(a, b) reset_command_list_(__LINE__, a, b)
static void reset_command_list_(unsigned int line,
        ID3D12GraphicsCommandList *list, ID3D12CommandAllocator *allocator)
{
    HRESULT hr;

    hr = ID3D12CommandAllocator_Reset(allocator);
    ok_(line)(SUCCEEDED(hr), "Failed to reset command allocator, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(list, allocator, NULL);
    ok_(line)(SUCCEEDED(hr), "Failed to reset command list, hr %#x.\n", hr);
}

#ifdef _WIN32
static HANDLE create_event(void)
{
    return CreateEventA(NULL, FALSE, FALSE, NULL);
}

static void signal_event(HANDLE event)
{
    SetEvent(event);
}

static unsigned int wait_event(HANDLE event, unsigned int milliseconds)
{
    return WaitForSingleObject(event, milliseconds);
}

static void destroy_event(HANDLE event)
{
    CloseHandle(event);
}
#else
static HANDLE create_event(void)
{
    return vkd3d_create_event();
}

static void signal_event(HANDLE event)
{
    vkd3d_signal_event(event);
}

static unsigned int wait_event(HANDLE event, unsigned int milliseconds)
{
    return vkd3d_wait_event(event, milliseconds);
}

static void destroy_event(HANDLE event)
{
    vkd3d_destroy_event(event);
}
#endif

typedef void (*thread_main_pfn)(void *data);

struct test_thread_data
{
    thread_main_pfn main_pfn;
    void *user_data;
};

#ifdef _WIN32
static DWORD WINAPI test_thread_main(void *untyped_data)
{
    struct test_thread_data *data = untyped_data;
    data->main_pfn(data->user_data);
    free(untyped_data);
    return 0;
}

static HANDLE create_thread(thread_main_pfn main_pfn, void *user_data)
{
    struct test_thread_data *data;

    if (!(data = malloc(sizeof(*data))))
        return NULL;
    data->main_pfn = main_pfn;
    data->user_data = user_data;

    return CreateThread(NULL, 0, test_thread_main, data, 0, NULL);
}

static bool join_thread(HANDLE thread)
{
    int ret;

    ret = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return ret == WAIT_OBJECT_0;
}
#else
static void *test_thread_main(void *untyped_data)
{
    struct test_thread_data *data = untyped_data;
    data->main_pfn(data->user_data);
    free(untyped_data);
    return NULL;
}

static HANDLE create_thread(thread_main_pfn main_pfn, void *user_data)
{
    struct test_thread_data *data;
    pthread_t *thread;

    if (!(thread = malloc(sizeof(*thread))))
        return NULL;

    if (!(data = malloc(sizeof(*data))))
    {
        free(thread);
        return NULL;
    }
    data->main_pfn = main_pfn;
    data->user_data = user_data;

    if (pthread_create(thread, NULL, test_thread_main, data))
    {
        free(data);
        free(thread);
        return NULL;
    }

    return thread;
}

static bool join_thread(HANDLE untyped_thread)
{
    pthread_t *thread = untyped_thread;
    int rc;

    rc = pthread_join(*thread, NULL);
    free(thread);
    return !rc;
}
#endif

static HRESULT wait_for_fence(ID3D12Fence *fence, UINT64 value)
{
    unsigned int ret;
    HANDLE event;
    HRESULT hr;

    if (ID3D12Fence_GetCompletedValue(fence) >= value)
        return S_OK;

    if (!(event = create_event()))
        return E_FAIL;

    if (FAILED(hr = ID3D12Fence_SetEventOnCompletion(fence, value, event)))
    {
        destroy_event(event);
        return hr;
    }

    ret = wait_event(event, INFINITE);
    destroy_event(event);

    return ret == WAIT_OBJECT_0;
}

#define wait_queue_idle(a, b) wait_queue_idle_(__LINE__, a, b)
static void wait_queue_idle_(unsigned int line, ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ID3D12Fence *fence;
    HRESULT hr;

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok_(line)(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    hr = ID3D12CommandQueue_Signal(queue, fence, 1);
    ok_(line)(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);
    hr = wait_for_fence(fence, 1);
    ok_(line)(SUCCEEDED(hr), "Failed to wait for fence, hr %#x.\n", hr);

    ID3D12Fence_Release(fence);
}

#define update_buffer_data(a, b, c, d) update_buffer_data_(__LINE__, a, b, c, d)
static void update_buffer_data_(unsigned int line, ID3D12Resource *buffer,
        size_t offset, size_t size, const void *data)
{
    D3D12_RANGE range;
    HRESULT hr;
    void *ptr;

    range.Begin = range.End = 0;
    hr = ID3D12Resource_Map(buffer, 0, &range, &ptr);
    ok_(line)(SUCCEEDED(hr), "Failed to map buffer, hr %#x.\n", hr);
    memcpy((BYTE *)ptr + offset, data, size);
    ID3D12Resource_Unmap(buffer, 0, NULL);
}

#define create_buffer(a, b, c, d, e) create_buffer_(__LINE__, a, b, c, d, e)
static ID3D12Resource *create_buffer_(unsigned int line, ID3D12Device *device,
        D3D12_HEAP_TYPE heap_type, size_t size, D3D12_RESOURCE_FLAGS resource_flags,
        D3D12_RESOURCE_STATES initial_resource_state)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Resource *buffer;
    HRESULT hr;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = heap_type;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = size;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = resource_flags;

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties,
            D3D12_HEAP_FLAG_NONE, &resource_desc, initial_resource_state,
            NULL, &IID_ID3D12Resource, (void **)&buffer);
    ok_(line)(SUCCEEDED(hr), "Failed to create buffer, hr %#x.\n", hr);
    return buffer;
}

#define create_default_buffer(a, b, c, d) create_default_buffer_(__LINE__, a, b, c, d)
static ID3D12Resource *create_default_buffer_(unsigned int line, ID3D12Device *device,
        size_t size, D3D12_RESOURCE_FLAGS resource_flags, D3D12_RESOURCE_STATES initial_resource_state)
{
    return create_buffer_(line, device, D3D12_HEAP_TYPE_DEFAULT, size,
            resource_flags, initial_resource_state);
}

#define create_upload_buffer(a, b, c) create_upload_buffer_(__LINE__, a, b, c)
static ID3D12Resource *create_upload_buffer_(unsigned int line, ID3D12Device *device,
        size_t size, const void *data)
{
    ID3D12Resource *buffer;

    buffer = create_buffer_(line, device, D3D12_HEAP_TYPE_UPLOAD, size,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (data)
        update_buffer_data_(line, buffer, 0, size, data);
    return buffer;
}

#define create_readback_buffer(a, b) create_readback_buffer_(__LINE__, a, b)
static ID3D12Resource *create_readback_buffer_(unsigned int line, ID3D12Device *device,
        size_t size)
{
    return create_buffer_(line, device, D3D12_HEAP_TYPE_READBACK, size,
            D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
}

#define create_texture(a, b, c, d, e) create_default_texture_(__LINE__, a, b, c, d, 0, e)
#define create_default_texture(a, b, c, d, e, f) create_default_texture_(__LINE__, a, b, c, d, e, f)
static ID3D12Resource *create_default_texture_(unsigned int line, ID3D12Device *device,
        unsigned int width, unsigned int height, DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial_state)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Resource *texture;
    HRESULT hr;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    memset(&resource_desc, 0, sizeof(resource_desc));
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = width;
    resource_desc.Height = height;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = format;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Flags = flags;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, initial_state, NULL, &IID_ID3D12Resource, (void **)&texture);
    ok_(line)(SUCCEEDED(hr), "Failed to create texture, hr %#x.\n", hr);

    return texture;
}

static void copy_sub_resource_data(const D3D12_MEMCPY_DEST *dst, const D3D12_SUBRESOURCE_DATA *src,
        unsigned int row_count, unsigned int slice_count, size_t row_size)
{
    const BYTE *src_slice_ptr;
    BYTE *dst_slice_ptr;
    unsigned int z, y;

    for (z = 0; z < slice_count; ++z)
    {
        dst_slice_ptr = (BYTE *)dst->pData + z * dst->SlicePitch;
        src_slice_ptr = (const BYTE*)src->pData + z * src->SlicePitch;
        for (y = 0; y < row_count; ++y)
            memcpy(dst_slice_ptr + y * dst->RowPitch, src_slice_ptr + y * src->RowPitch, row_size);
    }
}

#define upload_buffer_data(a, b, c, d, e, f) upload_buffer_data_(__LINE__, a, b, c, d, e, f)
static void upload_buffer_data_(unsigned int line, ID3D12Resource *buffer, size_t offset,
        size_t size, const void *data, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list)
{
    ID3D12Resource *upload_buffer;
    ID3D12Device *device;
    HRESULT hr;

    hr = ID3D12Resource_GetDevice(buffer, &IID_ID3D12Device, (void **)&device);
    ok_(line)(SUCCEEDED(hr), "Failed to get device, hr %#x.\n", hr);

    upload_buffer = create_upload_buffer_(line, device, size, data);

    ID3D12GraphicsCommandList_CopyBufferRegion(command_list, buffer, offset,
            upload_buffer, 0, size);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok_(line)(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);

    ID3D12Resource_Release(upload_buffer);
    ID3D12Device_Release(device);
}

#define upload_texture_data(a, b, c, d, e) upload_texture_data_(__LINE__, a, b, c, d, e)
static void upload_texture_data_(unsigned int line, ID3D12Resource *texture,
        const D3D12_SUBRESOURCE_DATA *data, unsigned int sub_resource_count,
        ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list)
{
    D3D12_TEXTURE_COPY_LOCATION dst_location, src_location;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts;
    D3D12_RESOURCE_DESC resource_desc;
    UINT64 *row_sizes, required_size;
    ID3D12Resource *upload_buffer;
    D3D12_MEMCPY_DEST dst_data;
    ID3D12Device *device;
    UINT *row_counts;
    unsigned int i;
    HRESULT hr;
    void *ptr;

    layouts = calloc(sub_resource_count, sizeof(*layouts));
    ok(layouts, "Failed to allocate memory.\n");
    row_counts = calloc(sub_resource_count, sizeof(*row_counts));
    ok(row_counts, "Failed to allocate memory.\n");
    row_sizes = calloc(sub_resource_count, sizeof(*row_sizes));
    ok(row_sizes, "Failed to allocate memory.\n");

    resource_desc = ID3D12Resource_GetDesc(texture);
    hr = ID3D12Resource_GetDevice(texture, &IID_ID3D12Device, (void **)&device);
    ok_(line)(SUCCEEDED(hr), "Failed to get device, hr %#x.\n", hr);

    ID3D12Device_GetCopyableFootprints(device, &resource_desc, 0, sub_resource_count,
            0, layouts, row_counts, row_sizes, &required_size);

    upload_buffer = create_upload_buffer_(line, device, required_size, NULL);

    hr = ID3D12Resource_Map(upload_buffer, 0, NULL, (void **)&ptr);
    ok_(line)(SUCCEEDED(hr), "Failed to map upload buffer, hr %#x.\n", hr);
    for (i = 0; i < sub_resource_count; ++i)
    {
        dst_data.pData = (BYTE *)ptr + layouts[i].Offset;
        dst_data.RowPitch = layouts[i].Footprint.RowPitch;
        dst_data.SlicePitch = layouts[i].Footprint.RowPitch * row_counts[i];
        copy_sub_resource_data(&dst_data, &data[i],
                row_counts[i], layouts[i].Footprint.Depth, row_sizes[i]);
    }
    ID3D12Resource_Unmap(upload_buffer, 0, NULL);

    for (i = 0; i < sub_resource_count; ++i)
    {
        dst_location.pResource = texture;
        dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_location.SubresourceIndex = i;

        src_location.pResource = upload_buffer;
        src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_location.PlacedFootprint = layouts[i];

        ID3D12GraphicsCommandList_CopyTextureRegion(command_list,
                &dst_location, 0, 0, 0, &src_location, NULL);
    }

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok_(line)(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);

    ID3D12Resource_Release(upload_buffer);
    ID3D12Device_Release(device);

    free(layouts);
    free(row_counts);
    free(row_sizes);
}

static unsigned int format_size(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_UNKNOWN:
            return 1;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
            return 16;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return 4;
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
            return 2;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 8;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16;
        default:
            trace("Unhandled format %#x.\n", format);
            return 1;
    }
}

static unsigned int format_block_width(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 4;
        default:
            return 1;
    }
}

static unsigned int format_block_height(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 4;
        default:
            return 1;
    }
}

struct resource_readback
{
    unsigned int width;
    unsigned int height;
    ID3D12Resource *resource;
    unsigned int row_pitch;
    void *data;
};

static void get_buffer_readback_with_command_list(ID3D12Resource *buffer, DXGI_FORMAT format,
        struct resource_readback *rb, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Resource *rb_buffer;
    D3D12_RANGE read_range;
    ID3D12Device *device;
    HRESULT hr;

    hr = ID3D12Resource_GetDevice(buffer, &IID_ID3D12Device, (void **)&device);
    ok(SUCCEEDED(hr), "Failed to get device, hr %#x.\n", hr);

    resource_desc = ID3D12Resource_GetDesc(buffer);
    assert(resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
    resource_desc.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    hr = ID3D12Resource_GetHeapProperties(buffer, &heap_properties, NULL);
    ok(SUCCEEDED(hr), "Failed to get heap properties.\n");
    if (heap_properties.Type == D3D12_HEAP_TYPE_READBACK)
    {
        rb_buffer = buffer;
        ID3D12Resource_AddRef(rb_buffer);
    }
    else
    {
        rb_buffer = create_readback_buffer(device, resource_desc.Width);
        ID3D12GraphicsCommandList_CopyBufferRegion(command_list, rb_buffer, 0,
                buffer, 0, resource_desc.Width);
    }

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Failed to close command list, hr %#x.\n", hr);

    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);
    ID3D12Device_Release(device);

    rb->width = resource_desc.Width / format_size(format);
    rb->height = 1;
    rb->resource = rb_buffer;
    rb->row_pitch = resource_desc.Width;
    rb->data = NULL;

    read_range.Begin = 0;
    read_range.End = resource_desc.Width;
    hr = ID3D12Resource_Map(rb_buffer, 0, &read_range, &rb->data);
    ok(SUCCEEDED(hr), "Failed to map readback buffer, hr %#x.\n", hr);
}

static void get_texture_readback_with_command_list(ID3D12Resource *texture, unsigned int sub_resource,
        struct resource_readback *rb, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list)
{
    D3D12_TEXTURE_COPY_LOCATION dst_location, src_location;
    D3D12_RESOURCE_DESC resource_desc;
    D3D12_RANGE read_range;
    unsigned int miplevel;
    ID3D12Device *device;
    DXGI_FORMAT format;
    HRESULT hr;

    hr = ID3D12Resource_GetDevice(texture, &IID_ID3D12Device, (void **)&device);
    ok(SUCCEEDED(hr), "Failed to get device, hr %#x.\n", hr);

    resource_desc = ID3D12Resource_GetDesc(texture);
    ok(resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER,
            "Resource %p is not texture.\n", texture);
    ok(resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D,
            "Readback not implemented for 3D textures.\n");

    miplevel = sub_resource % resource_desc.MipLevels;
    rb->width = max(1, resource_desc.Width >> miplevel);
    rb->height = max(1, resource_desc.Height >> miplevel);
    rb->row_pitch = align(rb->width * format_size(resource_desc.Format), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    rb->data = NULL;

    format = resource_desc.Format;

    rb->resource = create_readback_buffer(device, rb->row_pitch * rb->height);

    dst_location.pResource = rb->resource;
    dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_location.PlacedFootprint.Offset = 0;
    dst_location.PlacedFootprint.Footprint.Format = format;
    dst_location.PlacedFootprint.Footprint.Width = rb->width;
    dst_location.PlacedFootprint.Footprint.Height = rb->height;
    dst_location.PlacedFootprint.Footprint.Depth = 1;
    dst_location.PlacedFootprint.Footprint.RowPitch = rb->row_pitch;

    src_location.pResource = texture;
    src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_location.SubresourceIndex = sub_resource;

    ID3D12GraphicsCommandList_CopyTextureRegion(command_list, &dst_location, 0, 0, 0, &src_location, NULL);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Failed to close command list, hr %#x.\n", hr);

    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);
    ID3D12Device_Release(device);

    read_range.Begin = 0;
    read_range.End = resource_desc.Width;
    hr = ID3D12Resource_Map(rb->resource, 0, &read_range, &rb->data);
    ok(SUCCEEDED(hr), "Failed to map readback buffer, hr %#x.\n", hr);
}

static void *get_readback_data(struct resource_readback *rb, unsigned int x, unsigned int y,
        size_t element_size)
{
    return &((BYTE *)rb->data)[rb->row_pitch * y + x * element_size];
}

static uint16_t get_readback_uint16(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return *(uint16_t *)get_readback_data(rb, x, y, sizeof(uint16_t));
}

static unsigned int get_readback_uint(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return *(unsigned int *)get_readback_data(rb, x, y, sizeof(unsigned int));
}

static UINT64 get_readback_uint64(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return *(UINT64 *)get_readback_data(rb, x, y, sizeof(UINT64));
}

static float get_readback_float(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return *(float *)get_readback_data(rb, x, y, sizeof(float));
}

static const struct vec4 *get_readback_vec4(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return get_readback_data(rb, x, y, sizeof(struct vec4));
}

static const struct uvec4 *get_readback_uvec4(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return get_readback_data(rb, x, y, sizeof(struct uvec4));
}

static void release_resource_readback(struct resource_readback *rb)
{
    D3D12_RANGE range = {0, 0};
    ID3D12Resource_Unmap(rb->resource, 0, &range);
    ID3D12Resource_Release(rb->resource);
}

#define check_readback_data_float(a, b, c, d) check_readback_data_float_(__LINE__, a, b, c, d)
static void check_readback_data_float_(unsigned int line, struct resource_readback *rb,
        const RECT *rect, float expected, unsigned int max_diff)
{
    RECT r = {0, 0, rb->width, rb->height};
    unsigned int x = 0, y;
    bool all_match = true;
    float got = 0;

    if (rect)
        r = *rect;

    for (y = r.top; y < r.bottom; ++y)
    {
        for (x = r.left; x < r.right; ++x)
        {
            got = get_readback_float(rb, x, y);
            if (!compare_float(got, expected, max_diff))
            {
                all_match = false;
                break;
            }
        }
        if (!all_match)
            break;
    }
    ok_(line)(all_match, "Got %.8e, expected %.8e at (%u, %u).\n", got, expected, x, y);
}

#define check_sub_resource_float(a, b, c, d, e, f) check_sub_resource_float_(__LINE__, a, b, c, d, e, f)
static void check_sub_resource_float_(unsigned int line, ID3D12Resource *texture,
        unsigned int sub_resource_idx, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list,
        float expected, unsigned int max_diff)
{
    struct resource_readback rb;

    get_texture_readback_with_command_list(texture, 0, &rb, queue, command_list);
    check_readback_data_float_(line, &rb, NULL, expected, max_diff);
    release_resource_readback(&rb);
}

#define check_readback_data_uint16(a, b, c, d) check_readback_data_uint16_(__LINE__, a, b, c, d)
static void check_readback_data_uint16_(unsigned int line, struct resource_readback *rb,
        const RECT *rect, uint16_t expected, unsigned int max_diff)
{
    RECT r = {0, 0, rb->width, rb->height};
    unsigned int x = 0, y;
    bool all_match = true;
    uint16_t got = 0;

    if (rect)
        r = *rect;

    for (y = r.top; y < r.bottom; ++y)
    {
        for (x = r.left; x < r.right; ++x)
        {
            got = get_readback_uint16(rb, x, y);
            if (!compare_uint16(got, expected, max_diff))
            {
                all_match = false;
                break;
            }
        }
        if (!all_match)
            break;
    }
    ok_(line)(all_match, "Got 0x%04x, expected 0x%04x at (%u, %u).\n", got, expected, x, y);
}

#define check_sub_resource_uint16(a, b, c, d, e, f) check_sub_resource_uint16_(__LINE__, a, b, c, d, e, f)
static void check_sub_resource_uint16_(unsigned int line, ID3D12Resource *texture,
        unsigned int sub_resource_idx, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list,
        uint16_t expected, unsigned int max_diff)
{
    struct resource_readback rb;

    get_texture_readback_with_command_list(texture, 0, &rb, queue, command_list);
    check_readback_data_uint16_(line, &rb, NULL, expected, max_diff);
    release_resource_readback(&rb);
}

#define check_readback_data_uint(a, b, c, d) check_readback_data_uint_(__LINE__, a, b, c, d)
static void check_readback_data_uint_(unsigned int line, struct resource_readback *rb,
        const RECT *rect, unsigned int expected, unsigned int max_diff)
{
    RECT r = {0, 0, rb->width, rb->height};
    unsigned int x = 0, y;
    bool all_match = true;
    unsigned int got = 0;

    if (rect)
        r = *rect;

    for (y = r.top; y < r.bottom; ++y)
    {
        for (x = r.left; x < r.right; ++x)
        {
            got = get_readback_uint(rb, x, y);
            if (!compare_color(got, expected, max_diff))
            {
                all_match = false;
                break;
            }
        }
        if (!all_match)
            break;
    }
    ok_(line)(all_match, "Got 0x%08x, expected 0x%08x at (%u, %u).\n", got, expected, x, y);
}

#define check_sub_resource_uint(a, b, c, d, e, f) check_sub_resource_uint_(__LINE__, a, b, c, d, e, f)
static void check_sub_resource_uint_(unsigned int line, ID3D12Resource *texture,
        unsigned int sub_resource_idx, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list,
        unsigned int expected, unsigned int max_diff)
{
    struct resource_readback rb;

    get_texture_readback_with_command_list(texture, 0, &rb, queue, command_list);
    check_readback_data_uint_(line, &rb, NULL, expected, max_diff);
    release_resource_readback(&rb);
}

#define check_sub_resource_vec4(a, b, c, d, e, f) check_sub_resource_vec4_(__LINE__, a, b, c, d, e, f)
static void check_sub_resource_vec4_(unsigned int line, ID3D12Resource *texture,
        unsigned int sub_resource_idx, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list,
        const struct vec4 *expected, unsigned int max_diff)
{
    struct resource_readback rb;
    unsigned int x = 0, y;
    bool all_match = true;
    struct vec4 got = {};

    get_texture_readback_with_command_list(texture, 0, &rb, queue, command_list);
    for (y = 0; y < rb.height; ++y)
    {
        for (x = 0; x < rb.width; ++x)
        {
            got = *get_readback_vec4(&rb, x, y);
            if (!compare_vec4(&got, expected, max_diff))
            {
                all_match = false;
                break;
            }
        }
        if (!all_match)
            break;
    }
    release_resource_readback(&rb);

    ok_(line)(all_match, "Got {%.8e, %.8e, %.8e, %.8e}, expected {%.8e, %.8e, %.8e, %.8e} at (%u, %u).\n",
            got.x, got.y, got.z, got.w, expected->x, expected->y, expected->z, expected->w, x, y);
}

#ifdef _WIN32
static IUnknown *create_warp_adapter(void)
{
    IDXGIFactory4 *factory;
    IUnknown *adapter;
    HRESULT hr;

    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory);
    ok(SUCCEEDED(hr), "Failed to create IDXGIFactory4, hr %#x.\n", hr);

    adapter = NULL;
    hr = IDXGIFactory4_EnumWarpAdapter(factory, &IID_IUnknown, (void **)&adapter);
    IDXGIFactory4_Release(factory);
    if (FAILED(hr))
        trace("Failed to get WARP adapter, hr %#x.\n", hr);
    return adapter;
}
#else
static IUnknown *create_warp_adapter(void)
{
    return NULL;
}
#endif

static bool use_warp_device;

static ID3D12Device *create_device(void)
{
    IUnknown *adapter = NULL;
    ID3D12Device *device;

    if (use_warp_device && !(adapter = create_warp_adapter()))
    {
        trace("Failed to create WARP device.\n");
        return NULL;
    }

    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device)))
        return NULL;

    return device;
}

static bool is_min_max_filtering_supported(ID3D12Device *device)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS options;
    HRESULT hr;

    if (FAILED(hr = ID3D12Device_CheckFeatureSupport(device,
            D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
    {
        trace("Failed to check feature support, hr %#x.\n", hr);
        return false;
    }

    /* D3D12 validation layer says tiled resource tier 2+ support implies min/max filtering support. */
    return options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
}

#define create_empty_root_signature(device, flags) create_empty_root_signature_(__LINE__, device, flags)
static ID3D12RootSignature *create_empty_root_signature_(unsigned int line,
        ID3D12Device *device, D3D12_ROOT_SIGNATURE_FLAGS flags)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature = NULL;
    HRESULT hr;

    root_signature_desc.NumParameters = 0;
    root_signature_desc.pParameters = NULL;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = flags;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok_(line)(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    return root_signature;
}

#define create_cb_root_signature(a, b, c, e) create_cb_root_signature_(__LINE__, a, b, c, e)
static ID3D12RootSignature *create_cb_root_signature_(unsigned int line,
        ID3D12Device *device, unsigned int reg_idx, D3D12_SHADER_VISIBILITY shader_visibility,
        D3D12_ROOT_SIGNATURE_FLAGS flags)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature = NULL;
    D3D12_ROOT_PARAMETER root_parameter;
    HRESULT hr;

    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameter.Descriptor.ShaderRegister = reg_idx;
    root_parameter.Descriptor.RegisterSpace = 0;
    root_parameter.ShaderVisibility = shader_visibility;

    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = &root_parameter;
    root_signature_desc.Flags = flags;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok_(line)(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    return root_signature;
}

#define create_32bit_constants_root_signature(a, b, c, e) \
        create_32bit_constants_root_signature_(__LINE__, a, b, c, e)
static ID3D12RootSignature *create_32bit_constants_root_signature_(unsigned int line,
        ID3D12Device *device, unsigned int reg_idx, unsigned int element_count,
        D3D12_SHADER_VISIBILITY shader_visibility)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature = NULL;
    D3D12_ROOT_PARAMETER root_parameter;
    HRESULT hr;

    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameter.Constants.ShaderRegister = reg_idx;
    root_parameter.Constants.RegisterSpace = 0;
    root_parameter.Constants.Num32BitValues = element_count;
    root_parameter.ShaderVisibility = shader_visibility;

    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = &root_parameter;
    root_signature_desc.Flags = 0;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok_(line)(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    return root_signature;
}

#define create_texture_root_signature(a, b, c, d) create_texture_root_signature_(__LINE__, a, b, c, d)
static ID3D12RootSignature *create_texture_root_signature_(unsigned int line,
        ID3D12Device *device, D3D12_SHADER_VISIBILITY shader_visibility,
        unsigned int constant_count, D3D12_ROOT_SIGNATURE_FLAGS flags)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature = NULL;
    D3D12_DESCRIPTOR_RANGE descriptor_range;
    D3D12_ROOT_PARAMETER root_parameters[2];
    D3D12_STATIC_SAMPLER_DESC sampler_desc;
    HRESULT hr;

    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.ShaderRegister = 0;
    sampler_desc.RegisterSpace = 0;
    sampler_desc.ShaderVisibility = shader_visibility;

    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range.NumDescriptors = 1;
    descriptor_range.BaseShaderRegister = 0;
    descriptor_range.RegisterSpace = 0;
    descriptor_range.OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &descriptor_range;
    root_parameters[0].ShaderVisibility = shader_visibility;

    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 0;
    root_parameters[1].Constants.RegisterSpace = 0;
    root_parameters[1].Constants.Num32BitValues = constant_count;
    root_parameters[1].ShaderVisibility = shader_visibility;

    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = constant_count ? 2 : 1;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 1;
    root_signature_desc.pStaticSamplers = &sampler_desc;
    root_signature_desc.Flags = flags;

    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok_(line)(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    return root_signature;
}

#define create_compute_pipeline_state(a, b, c) create_compute_pipeline_state_(__LINE__, a, b, c)
static ID3D12PipelineState *create_compute_pipeline_state_(unsigned int line, ID3D12Device *device,
        ID3D12RootSignature *root_signature, const D3D12_SHADER_BYTECODE cs)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_state_desc;
    ID3D12PipelineState *pipeline_state;
    HRESULT hr;

    memset(&pipeline_state_desc, 0, sizeof(pipeline_state_desc));
    pipeline_state_desc.pRootSignature = root_signature;
    pipeline_state_desc.CS = cs;
    pipeline_state_desc.NodeMask = 0;
    pipeline_state_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    hr = ID3D12Device_CreateComputePipelineState(device, &pipeline_state_desc,
            &IID_ID3D12PipelineState, (void **)&pipeline_state);
    ok_(line)(SUCCEEDED(hr), "Failed to create compute pipeline state, hr %#x.\n", hr);

    return pipeline_state;
}

static void init_pipeline_state_desc(D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
        ID3D12RootSignature *root_signature, DXGI_FORMAT rt_format,
        const D3D12_SHADER_BYTECODE *vs, const D3D12_SHADER_BYTECODE *ps,
        const D3D12_INPUT_LAYOUT_DESC *input_layout)
{
    static const DWORD vs_code[] =
    {
#if 0
        void main(uint id : SV_VertexID, out float4 position : SV_Position)
        {
            float2 coords = float2((id << 1) & 2, id & 2);
            position = float4(coords * float2(2, -2) + float2(-1, 1), 0, 1);
        }
#endif
        0x43425844, 0xf900d25e, 0x68bfefa7, 0xa63ac0a7, 0xa476af7a, 0x00000001, 0x0000018c, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000006, 0x00000001, 0x00000000, 0x00000101, 0x565f5653, 0x65747265, 0x00444978,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000001, 0x00000003,
        0x00000000, 0x0000000f, 0x505f5653, 0x7469736f, 0x006e6f69, 0x58454853, 0x000000f0, 0x00010050,
        0x0000003c, 0x0100086a, 0x04000060, 0x00101012, 0x00000000, 0x00000006, 0x04000067, 0x001020f2,
        0x00000000, 0x00000001, 0x02000068, 0x00000001, 0x0b00008c, 0x00100012, 0x00000000, 0x00004001,
        0x00000001, 0x00004001, 0x00000001, 0x0010100a, 0x00000000, 0x00004001, 0x00000000, 0x07000001,
        0x00100042, 0x00000000, 0x0010100a, 0x00000000, 0x00004001, 0x00000002, 0x05000056, 0x00100032,
        0x00000000, 0x00100086, 0x00000000, 0x0f000032, 0x00102032, 0x00000000, 0x00100046, 0x00000000,
        0x00004002, 0x40000000, 0xc0000000, 0x00000000, 0x00000000, 0x00004002, 0xbf800000, 0x3f800000,
        0x00000000, 0x00000000, 0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x3f800000, 0x0100003e,
    };
    static const DWORD ps_code[] =
    {
#if 0
        void main(const in float4 position : SV_Position, out float4 target : SV_Target0)
        {
            target = float4(0.0f, 1.0f, 0.0f, 1.0f);
        }
#endif
        0x43425844, 0x8a4a8140, 0x5eba8e0b, 0x714e0791, 0xb4b8eed2, 0x00000001, 0x000000d8, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000003c, 0x00000050,
        0x0000000f, 0x0100086a, 0x03000065, 0x001020f2, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000, 0x0100003e,
    };

    memset(desc, 0, sizeof(*desc));
    desc->pRootSignature = root_signature;
    if (vs)
        desc->VS = *vs;
    else
        desc->VS = shader_bytecode(vs_code, sizeof(vs_code));
    if (ps)
        desc->PS = *ps;
    else
        desc->PS = shader_bytecode(ps_code, sizeof(ps_code));
    desc->StreamOutput.RasterizedStream = 0;
    desc->BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc->RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (input_layout)
        desc->InputLayout = *input_layout;
    desc->SampleMask = ~(UINT)0;
    desc->PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc->NumRenderTargets = 1;
    desc->RTVFormats[0] = rt_format;
    desc->SampleDesc.Count = 1;
}

#define create_pipeline_state(a, b, c, d, e, f) create_pipeline_state_(__LINE__, a, b, c, d, e, f)
static ID3D12PipelineState *create_pipeline_state_(unsigned int line, ID3D12Device *device,
        ID3D12RootSignature *root_signature, DXGI_FORMAT rt_format,
        const D3D12_SHADER_BYTECODE *vs, const D3D12_SHADER_BYTECODE *ps,
        const D3D12_INPUT_LAYOUT_DESC *input_layout)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_state_desc;
    ID3D12PipelineState *pipeline_state;
    HRESULT hr;

    init_pipeline_state_desc(&pipeline_state_desc, root_signature, rt_format, vs, ps, input_layout);
    hr = ID3D12Device_CreateGraphicsPipelineState(device, &pipeline_state_desc,
            &IID_ID3D12PipelineState, (void **)&pipeline_state);
    ok_(line)(SUCCEEDED(hr), "Failed to create graphics pipeline state, hr %#x.\n", hr);

    return pipeline_state;
}

struct test_context_desc
{
    unsigned int rt_width, rt_height;
    DXGI_FORMAT rt_format;
    unsigned int rt_descriptor_count;
    bool no_render_target;
    bool no_root_signature;
    bool no_pipeline;
};

struct test_context
{
    ID3D12Device *device;

    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;

    D3D12_RESOURCE_DESC render_target_desc;
    ID3D12Resource *render_target;

    ID3D12DescriptorHeap *rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;

    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;

    D3D12_VIEWPORT viewport;
    RECT scissor_rect;
};

#define create_render_target(context, a, b, c) create_render_target_(__LINE__, context, a, b, c)
static void create_render_target_(unsigned int line, struct test_context *context,
        const struct test_context_desc *desc, ID3D12Resource **render_target,
        const D3D12_CPU_DESCRIPTOR_HANDLE *rtv)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    D3D12_CLEAR_VALUE clear_value;
    HRESULT hr;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = desc && desc->rt_width ? desc->rt_width : 32;
    resource_desc.Height = desc && desc->rt_height ? desc->rt_height : 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = desc && desc->rt_format ? desc->rt_format : DXGI_FORMAT_R8G8B8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear_value.Format = resource_desc.Format;
    clear_value.Color[0] = 1.0f;
    clear_value.Color[1] = 1.0f;
    clear_value.Color[2] = 1.0f;
    clear_value.Color[3] = 1.0f;
    hr = ID3D12Device_CreateCommittedResource(context->device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            &IID_ID3D12Resource, (void **)render_target);
    ok_(line)(SUCCEEDED(hr), "Failed to create texture, hr %#x.\n", hr);

    context->render_target_desc = resource_desc;

    if (rtv)
        ID3D12Device_CreateRenderTargetView(context->device, *render_target, NULL, *rtv);
}

#define init_test_context(context, desc) init_test_context_(__LINE__, context, desc)
static bool init_test_context_(unsigned int line, struct test_context *context,
        const struct test_context_desc *desc)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
    ID3D12Device *device;
    HRESULT hr;

    memset(context, 0, sizeof(*context));

    if (!(context->device = create_device()))
    {
        skip_(line)("Failed to create device.\n");
        return false;
    }
    device = context->device;

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&context->queue);
    ok_(line)(SUCCEEDED(hr), "Failed to create command queue, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&context->allocator);
    ok_(line)(SUCCEEDED(hr), "Failed to create command allocator, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            context->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&context->list);
    ok_(line)(SUCCEEDED(hr), "Failed to create command list, hr %#x.\n", hr);

    if (desc && desc->no_render_target)
        return true;

    rtv_heap_desc.NumDescriptors = desc && desc->rt_descriptor_count ? desc->rt_descriptor_count : 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &rtv_heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&context->rtv_heap);
    ok_(line)(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    context->rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(context->rtv_heap);

    create_render_target_(line, context, desc, &context->render_target, &context->rtv);

    set_viewport(&context->viewport, 0.0f, 0.0f,
            context->render_target_desc.Width, context->render_target_desc.Height, 0.0f, 1.0f);
    set_rect(&context->scissor_rect, 0, 0,
            context->render_target_desc.Width, context->render_target_desc.Height);

    if (desc && desc->no_root_signature)
        return true;

    context->root_signature = create_empty_root_signature_(line,
            device, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    if (desc && desc->no_pipeline)
        return true;

    context->pipeline_state = create_pipeline_state_(line, device,
            context->root_signature, context->render_target_desc.Format,
            NULL, NULL, NULL);

    return true;
}

#define init_compute_test_context(context) init_compute_test_context_(__LINE__, context)
static bool init_compute_test_context_(unsigned int line, struct test_context *context)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12Device *device;
    HRESULT hr;

    memset(context, 0, sizeof(*context));

    if (!(context->device = create_device()))
    {
        skip_(line)("Failed to create device.\n");
        return false;
    }
    device = context->device;

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&context->queue);
    ok_(line)(SUCCEEDED(hr), "Failed to create command queue, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            &IID_ID3D12CommandAllocator, (void **)&context->allocator);
    ok_(line)(SUCCEEDED(hr), "Failed to create command allocator, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            context->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&context->list);
    ok_(line)(SUCCEEDED(hr), "Failed to create command list, hr %#x.\n", hr);

    return true;
}

#define destroy_test_context(context) destroy_test_context_(__LINE__, context)
static void destroy_test_context_(unsigned int line, struct test_context *context)
{
    ULONG refcount;

    if (context->pipeline_state)
        ID3D12PipelineState_Release(context->pipeline_state);
    if (context->root_signature)
        ID3D12RootSignature_Release(context->root_signature);

    if (context->rtv_heap)
        ID3D12DescriptorHeap_Release(context->rtv_heap);
    if (context->render_target)
        ID3D12Resource_Release(context->render_target);

    ID3D12CommandAllocator_Release(context->allocator);
    ID3D12CommandQueue_Release(context->queue);
    ID3D12GraphicsCommandList_Release(context->list);

    refcount = ID3D12Device_Release(context->device);
    ok_(line)(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_handle(ID3D12Device *device,
        ID3D12DescriptorHeap *heap, D3D12_DESCRIPTOR_HEAP_TYPE heap_type, unsigned int offset)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    unsigned int descriptor_size;

    handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    if (!offset)
        return handle;

    descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(device, heap_type);
    handle.ptr += offset * descriptor_size;
    return handle;
}

static D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_descriptor_handle(struct test_context *context,
        ID3D12DescriptorHeap *heap, unsigned int offset)
{
    return get_cpu_handle(context->device, heap, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, offset);
}

static D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_sampler_handle(struct test_context *context,
        ID3D12DescriptorHeap *heap, unsigned int offset)
{
    return get_cpu_handle(context->device, heap, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, offset);
}

static D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_rtv_handle(struct test_context *context,
        ID3D12DescriptorHeap *heap, unsigned int offset)
{
    return get_cpu_handle(context->device, heap, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, offset);
}

static D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_handle(ID3D12Device *device,
        ID3D12DescriptorHeap *heap, D3D12_DESCRIPTOR_HEAP_TYPE heap_type, unsigned int offset)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    unsigned int descriptor_size;

    handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    if (!offset)
        return handle;

    descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(device, heap_type);
    handle.ptr += offset * descriptor_size;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_descriptor_handle(struct test_context *context,
        ID3D12DescriptorHeap *heap, unsigned int offset)
{
    return get_gpu_handle(context->device, heap, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, offset);
}

static D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_sampler_handle(struct test_context *context,
        ID3D12DescriptorHeap *heap, unsigned int offset)
{
    return get_gpu_handle(context->device, heap, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, offset);
}

struct depth_stencil_resource
{
    ID3D12Resource *texture;
    ID3D12DescriptorHeap *heap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
};

#define init_depth_stencil(a, b, c, d, e, f, g) init_depth_stencil_(__LINE__, a, b, c, d, e, f, g)
static void init_depth_stencil_(unsigned int line, struct depth_stencil_resource *ds,
        ID3D12Device *device, unsigned int width, unsigned int height,
        DXGI_FORMAT format, DXGI_FORMAT view_format, const D3D12_CLEAR_VALUE *clear_value)
{
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc, *view_desc;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    HRESULT hr;

    memset(ds, 0, sizeof(*ds));

    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsv_heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &dsv_heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&ds->heap);
    ok_(line)(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = width;
    resource_desc.Height = height;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = format;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, clear_value,
            &IID_ID3D12Resource, (void **)&ds->texture);
    ok_(line)(SUCCEEDED(hr), "Failed to create texture, hr %#x.\n", hr);

    view_desc = NULL;
    if (view_format)
    {
        memset(&dsv_desc, 0, sizeof(dsv_desc));
        dsv_desc.Format = view_format;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        view_desc = &dsv_desc;
    }
    ds->dsv_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(ds->heap);
    ID3D12Device_CreateDepthStencilView(device, ds->texture, view_desc, ds->dsv_handle);
}

#define destroy_depth_stencil(depth_stencil) destroy_depth_stencil_(__LINE__, depth_stencil)
static void destroy_depth_stencil_(unsigned int line, struct depth_stencil_resource *ds)
{
    ID3D12DescriptorHeap_Release(ds->heap);
    ID3D12Resource_Release(ds->texture);
}

static void test_create_device(void)
{
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    check_interface(device, &IID_ID3D12Object, TRUE);
    check_interface(device, &IID_ID3D12DeviceChild, FALSE);
    check_interface(device, &IID_ID3D12Pageable, FALSE);
    check_interface(device, &IID_ID3D12Device, TRUE);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    ok(hr == S_OK, "D3D12CreateDevice failed, hr %#x.\n", hr);
    ID3D12Device_Release(device);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_9_1, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_9_2, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_9_3, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_10_0, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_10_1, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = D3D12CreateDevice(NULL, 0, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "D3D12CreateDevice failed, hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, ~0u, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "D3D12CreateDevice failed, hr %#x.\n", hr);
}

static void test_node_count(void)
{
    ID3D12Device *device;
    UINT node_count;
    ULONG refcount;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    node_count = ID3D12Device_GetNodeCount(device);
    trace("Node count: %u.\n", node_count);
    ok(1 <= node_count && node_count <= 32, "Got unexpected node count %u.\n", node_count);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_check_feature_support(void)
{
    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels;
    D3D_FEATURE_LEVEL max_supported_feature_level;
    D3D12_FEATURE_DATA_ARCHITECTURE architecture;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    static const D3D_FEATURE_LEVEL all_feature_levels[] =
    {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    static const D3D_FEATURE_LEVEL d3d12_feature_levels[] =
    {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    static const D3D_FEATURE_LEVEL d3d_9_x_feature_levels[] =
    {
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    static const D3D_FEATURE_LEVEL invalid_feature_levels[] =
    {
        0x0000,
        0x3000,
    };

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    /* Architecture. */
    memset(&architecture, 0, sizeof(architecture));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_ARCHITECTURE,
            &architecture, sizeof(architecture));
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    ok(!architecture.NodeIndex, "Got unexpected node %u.\n", architecture.NodeIndex);
    ok(!architecture.CacheCoherentUMA || architecture.UMA,
            "Got unexpected cache coherent UMA %#x (UMA %#x).\n",
            architecture.CacheCoherentUMA, architecture.UMA);
    trace("UMA %#x, cache coherent UMA %#x, tile based renderer %#x.\n",
            architecture.UMA, architecture.CacheCoherentUMA, architecture.TileBasedRenderer);

    if (ID3D12Device_GetNodeCount(device) == 1)
    {
        memset(&architecture, 0, sizeof(architecture));
        architecture.NodeIndex = 1;
        hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_ARCHITECTURE,
                &architecture, sizeof(architecture));
        ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    }

    /* Feature levels */
    memset(&feature_levels, 0, sizeof(feature_levels));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    feature_levels.NumFeatureLevels = ARRAY_SIZE(all_feature_levels);
    feature_levels.pFeatureLevelsRequested = all_feature_levels;
    feature_levels.MaxSupportedFeatureLevel = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(SUCCEEDED(hr), "Failed to check feature support, hr %#x.\n", hr);
    trace("Max supported feature level %#x.\n", feature_levels.MaxSupportedFeatureLevel);
    max_supported_feature_level = feature_levels.MaxSupportedFeatureLevel;

    feature_levels.NumFeatureLevels = ARRAY_SIZE(d3d12_feature_levels);
    feature_levels.pFeatureLevelsRequested = d3d12_feature_levels;
    feature_levels.MaxSupportedFeatureLevel = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(SUCCEEDED(hr), "Failed to check feature support, hr %#x.\n", hr);
    ok(feature_levels.MaxSupportedFeatureLevel == max_supported_feature_level,
            "Got unexpected feature level %#x, expected %#x.\n",
            feature_levels.MaxSupportedFeatureLevel, max_supported_feature_level);

    /* Check invalid size. */
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels) + 1);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels) - 1);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    feature_levels.NumFeatureLevels = ARRAY_SIZE(d3d_9_x_feature_levels);
    feature_levels.pFeatureLevelsRequested = d3d_9_x_feature_levels;
    feature_levels.MaxSupportedFeatureLevel = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(SUCCEEDED(hr), "Failed to check feature support, hr %#x.\n", hr);
    ok(feature_levels.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_9_3,
            "Got unexpected max feature level %#x.\n", feature_levels.MaxSupportedFeatureLevel);

    feature_levels.NumFeatureLevels = ARRAY_SIZE(invalid_feature_levels);
    feature_levels.pFeatureLevelsRequested = invalid_feature_levels;
    feature_levels.MaxSupportedFeatureLevel = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(SUCCEEDED(hr), "Failed to check feature support, hr %#x.\n", hr);
    ok(feature_levels.MaxSupportedFeatureLevel == 0x3000,
            "Got unexpected max feature level %#x.\n", feature_levels.MaxSupportedFeatureLevel);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_format_support(void)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support;
    ID3D12Device *device;
    ULONG refcount;
    unsigned int i;
    HRESULT hr;

    static const D3D12_FEATURE_DATA_FORMAT_SUPPORT unsupported_format_features[] =
    {
        {DXGI_FORMAT_B8G8R8A8_TYPELESS, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW,
                D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE},
        {DXGI_FORMAT_B8G8R8A8_UNORM,    D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW,
                D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE},
    };

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    memset(&format_support, 0, sizeof(format_support));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_SUPPORT,
            &format_support, sizeof(format_support));
    todo(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    todo(format_support.Support1 == D3D12_FORMAT_SUPPORT1_BUFFER,
            "Got unexpected support1 %#x.\n", format_support.Support1);
    ok(!format_support.Support2 || format_support.Support2 == D3D12_FORMAT_SUPPORT2_TILED,
            "Got unexpected support2 %#x.\n", format_support.Support2);

    for (i = 0; i < ARRAY_SIZE(unsupported_format_features); ++i)
    {
        memset(&format_support, 0, sizeof(format_support));
        format_support.Format = unsupported_format_features[i].Format;
        hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_SUPPORT,
                &format_support, sizeof(format_support));
        ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
        ok(!(format_support.Support1 & unsupported_format_features[i].Support1),
                "Format %#x supports %#x.\n", unsupported_format_features[i].Format,
                format_support.Support1 & unsupported_format_features[i].Support1);
        ok(!(format_support.Support2 & unsupported_format_features[i].Support2),
                "Format %#x supports %#x.\n", unsupported_format_features[i].Format,
                format_support.Support2 & unsupported_format_features[i].Support2);
    }

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_command_allocator(void)
{
    ID3D12CommandAllocator *command_allocator;
    ID3D12Device *device, *tmp_device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12CommandAllocator_GetDevice(command_allocator, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(command_allocator, &IID_ID3D12Object, TRUE);
    check_interface(command_allocator, &IID_ID3D12DeviceChild, TRUE);
    check_interface(command_allocator, &IID_ID3D12Pageable, TRUE);
    check_interface(command_allocator, &IID_ID3D12CommandAllocator, TRUE);

    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COPY,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, ~0u,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(hr == E_INVALIDARG, "CreateCommandAllocator failed, hr %#x.\n", hr);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_command_list(void)
{
    ID3D12CommandAllocator *command_allocator;
    ID3D12Device *device, *tmp_device;
    ID3D12CommandList *command_list;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            NULL, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    refcount = get_refcount(command_allocator);
    ok(refcount == 1, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12CommandList_GetDevice(command_list, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 4, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(command_list, &IID_ID3D12Object, TRUE);
    check_interface(command_list, &IID_ID3D12DeviceChild, TRUE);
    check_interface(command_list, &IID_ID3D12Pageable, FALSE);
    check_interface(command_list, &IID_ID3D12CommandList, TRUE);
    check_interface(command_list, &IID_ID3D12GraphicsCommandList, TRUE);
    check_interface(command_list, &IID_ID3D12CommandAllocator, FALSE);

    refcount = ID3D12CommandList_Release(command_list);
    ok(!refcount, "ID3D12CommandList has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);
    check_interface(command_list, &IID_ID3D12GraphicsCommandList, TRUE);
    refcount = ID3D12CommandList_Release(command_list);
    ok(!refcount, "ID3D12CommandList has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);
    check_interface(command_list, &IID_ID3D12GraphicsCommandList, TRUE);
    refcount = ID3D12CommandList_Release(command_list);
    ok(!refcount, "ID3D12CommandList has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COPY,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_COPY,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);
    check_interface(command_list, &IID_ID3D12GraphicsCommandList, TRUE);
    refcount = ID3D12CommandList_Release(command_list);
    ok(!refcount, "ID3D12CommandList has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_command_queue(void)
{
    D3D12_COMMAND_QUEUE_DESC desc, result_desc;
    ID3D12Device *device, *tmp_device;
    ID3D12CommandQueue *queue;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12CommandQueue_GetDevice(queue, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(queue, &IID_ID3D12Object, TRUE);
    check_interface(queue, &IID_ID3D12DeviceChild, TRUE);
    check_interface(queue, &IID_ID3D12Pageable, TRUE);
    check_interface(queue, &IID_ID3D12CommandQueue, TRUE);

    result_desc = ID3D12CommandQueue_GetDesc(queue);
    ok(result_desc.Type == desc.Type, "Got unexpected type %#x.\n", result_desc.Type);
    ok(result_desc.Priority == desc.Priority, "Got unexpected priority %#x.\n", result_desc.Priority);
    ok(result_desc.Flags == desc.Flags, "Got unexpected flags %#x.\n", result_desc.Flags);
    ok(result_desc.NodeMask == 0x1, "Got unexpected node mask 0x%08x.\n", result_desc.NodeMask);

    refcount = ID3D12CommandQueue_Release(queue);
    ok(!refcount, "ID3D12CommandQueue has %u references left.\n", (unsigned int)refcount);

    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    hr = ID3D12Device_CreateCommandQueue(device, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    result_desc = ID3D12CommandQueue_GetDesc(queue);
    ok(result_desc.Type == desc.Type, "Got unexpected type %#x.\n", result_desc.Type);
    ok(result_desc.Priority == desc.Priority, "Got unexpected priority %#x.\n", result_desc.Priority);
    ok(result_desc.Flags == desc.Flags, "Got unexpected flags %#x.\n", result_desc.Flags);
    ok(result_desc.NodeMask == 0x1, "Got unexpected node mask 0x%08x.\n", result_desc.NodeMask);

    refcount = ID3D12CommandQueue_Release(queue);
    ok(!refcount, "ID3D12CommandQueue has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_committed_resource(void)
{
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Device *device, *tmp_device;
    D3D12_CLEAR_VALUE clear_value;
    ID3D12Resource *resource;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear_value.Color[0] = 1.0f;
    clear_value.Color[1] = 0.0f;
    clear_value.Color[2] = 0.0f;
    clear_value.Color[3] = 1.0f;

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12Resource_GetDevice(resource, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(resource, &IID_ID3D12Object, TRUE);
    check_interface(resource, &IID_ID3D12DeviceChild, TRUE);
    check_interface(resource, &IID_ID3D12Pageable, TRUE);
    check_interface(resource, &IID_ID3D12Resource, TRUE);

    gpu_address = ID3D12Resource_GetGPUVirtualAddress(resource);
    ok(!gpu_address, "Got unexpected GPU virtual address %#"PRIx64".\n", gpu_address);

    refcount = ID3D12Resource_Release(resource);
    ok(!refcount, "ID3D12Resource has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear_value, &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    /* For D3D12_RESOURCE_STATE_RENDER_TARGET the D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET flag is required. */
    resource_desc.Flags = 0;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    todo(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ID3D12Resource_Release(resource);

    /* A texture cannot be created on a UPLOAD heap. */
    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    /* A texture cannot be created on a READBACK heap. */
    heap_properties.Type = D3D12_HEAP_TYPE_READBACK;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    /* DXGI_FORMAT_B8G8R8A8_UNORM does not support D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS. */
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    todo(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ID3D12Resource_Release(resource);

    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    check_interface(resource, &IID_ID3D12Object, TRUE);
    check_interface(resource, &IID_ID3D12DeviceChild, TRUE);
    check_interface(resource, &IID_ID3D12Pageable, TRUE);
    check_interface(resource, &IID_ID3D12Resource, TRUE);

    gpu_address = ID3D12Resource_GetGPUVirtualAddress(resource);
    ok(gpu_address, "Got unexpected GPU virtual address %#"PRIx64".\n", gpu_address);

    refcount = ID3D12Resource_Release(resource);
    ok(!refcount, "ID3D12Resource has %u references left.\n", (unsigned int)refcount);

    /* The clear value must be NULL for buffers. */
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, &clear_value,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    /* For D3D12_HEAP_TYPE_UPLOAD the state must be D3D12_RESOURCE_STATE_GENERIC_READ. */
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    heap_properties.Type = D3D12_HEAP_TYPE_READBACK;

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);
    refcount = ID3D12Resource_Release(resource);
    ok(!refcount, "ID3D12Resource has %u references left.\n", (unsigned int)refcount);

    /* For D3D12_HEAP_TYPE_READBACK the state must be D3D12_RESOURCE_STATE_COPY_DEST. */
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_descriptor_heap(void)
{
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12Device *device, *tmp_device;
    ID3D12DescriptorHeap *heap;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 16;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12DescriptorHeap_GetDevice(heap, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(heap, &IID_ID3D12Object, TRUE);
    check_interface(heap, &IID_ID3D12DeviceChild, TRUE);
    check_interface(heap, &IID_ID3D12Pageable, TRUE);
    check_interface(heap, &IID_ID3D12DescriptorHeap, TRUE);

    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_sampler(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    unsigned int sampler_increment_size;
    D3D12_SAMPLER_DESC sampler_desc;
    ID3D12DescriptorHeap *heap;
    ID3D12Device *device;
    unsigned int i;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    sampler_increment_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    trace("Sampler descriptor handle increment size: %u.\n", sampler_increment_size);
    ok(sampler_increment_size, "Got unexpected increment size %#x.\n", sampler_increment_size);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap_desc.NumDescriptors = 16;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
    ID3D12Device_CreateSampler(device, &sampler_desc, cpu_handle);

    cpu_handle.ptr += sampler_increment_size;
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    for (i = 1; i < heap_desc.NumDescriptors; ++i)
    {
        ID3D12Device_CreateSampler(device, &sampler_desc, cpu_handle);
        cpu_handle.ptr += sampler_increment_size;
    }

    trace("MinMaxFiltering: %#x.\n", is_min_max_filtering_supported(device));
    if (is_min_max_filtering_supported(device))
    {
        cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
        sampler_desc.Filter = D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
        ID3D12Device_CreateSampler(device, &sampler_desc, cpu_handle);

        cpu_handle.ptr += sampler_increment_size;
        sampler_desc.Filter = D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_POINT;
        ID3D12Device_CreateSampler(device, &sampler_desc, cpu_handle);
    }

    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    sampler_desc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
    ID3D12Device_CreateSampler(device, &sampler_desc, cpu_handle);

    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_unordered_access_view(void)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    unsigned int descriptor_size;
    ID3D12DescriptorHeap *heap;
    ID3D12Resource *resource;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    trace("CBV/SRV/UAV descriptor size: %u.\n", descriptor_size);
    ok(descriptor_size, "Got unexpected descriptor size %#x.\n", descriptor_size);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 16;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    resource = create_default_buffer(device, 64 * sizeof(float),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = 64;
    uav_desc.Buffer.StructureByteStride = 0;
    uav_desc.Buffer.CounterOffsetInBytes = 0;
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    ID3D12Device_CreateUnorderedAccessView(device, resource, NULL, &uav_desc, cpu_handle);

    cpu_handle.ptr += descriptor_size;

    /* The following call fails. Buffer views cannot be created for compressed formats. */
    uav_desc.Format = DXGI_FORMAT_BC1_UNORM;
    ID3D12Device_CreateUnorderedAccessView(device, resource, NULL, &uav_desc, cpu_handle);

    ID3D12Resource_Release(resource);
    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_root_signature(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_ROOT_PARAMETER root_parameters[3];
    ID3D12RootSignature *root_signature;
    ID3D12Device *device, *tmp_device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    /* descriptor table */
    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12RootSignature_GetDevice(root_signature, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(root_signature, &IID_ID3D12Object, TRUE);
    check_interface(root_signature, &IID_ID3D12DeviceChild, TRUE);
    check_interface(root_signature, &IID_ID3D12Pageable, FALSE);
    check_interface(root_signature, &IID_ID3D12RootSignature, TRUE);

    refcount = ID3D12RootSignature_Release(root_signature);
    ok(!refcount, "ID3D12RootSignature has %u references left.\n", (unsigned int)refcount);

    /* empty */
    root_signature_desc.NumParameters = 0;
    root_signature_desc.pParameters = NULL;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);
    refcount = ID3D12RootSignature_Release(root_signature);
    ok(!refcount, "ID3D12RootSignature has %u references left.\n", (unsigned int)refcount);

    /* root constants */
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].Constants.ShaderRegister = 0;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = 4;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 0;
    root_parameters[1].Constants.RegisterSpace = 0;
    root_parameters[1].Constants.Num32BitValues = 8;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    todo(hr == E_FAIL || hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ID3D12RootSignature_Release(root_signature);
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);
    refcount = ID3D12RootSignature_Release(root_signature);
    ok(!refcount, "ID3D12RootSignature has %u references left.\n", (unsigned int)refcount);

    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[2].Constants.ShaderRegister = 1;
    root_parameters[2].Constants.RegisterSpace = 0;
    root_parameters[2].Constants.Num32BitValues = 3;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 3;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);
    refcount = ID3D12RootSignature_Release(root_signature);
    ok(!refcount, "ID3D12RootSignature has %u references left.\n", (unsigned int)refcount);

    /* root descriptors */
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[0].Descriptor.RegisterSpace = 0;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    todo(hr == E_FAIL || hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ID3D12RootSignature_Release(root_signature);
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);
    refcount = ID3D12RootSignature_Release(root_signature);
    ok(!refcount, "ID3D12RootSignature has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_root_signature_limits(void)
{
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[D3D12_MAX_ROOT_COST + 1];
    D3D12_ROOT_PARAMETER root_parameters[D3D12_MAX_ROOT_COST + 1];
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature;
    ID3D12Device *device;
    ULONG refcount;
    unsigned int i;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    /* A descriptor table costs 1 DWORD. */
    for (i = 0; i < ARRAY_SIZE(root_parameters); ++i)
    {
        descriptor_ranges[i].RangeType = i % 2
                ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptor_ranges[i].NumDescriptors = 1;
        descriptor_ranges[i].BaseShaderRegister = i / 2;
        descriptor_ranges[i].RegisterSpace = 0;
        descriptor_ranges[i].OffsetInDescriptorsFromTableStart = 0;
        root_parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_parameters[i].DescriptorTable.NumDescriptorRanges = 1;
        root_parameters[i].DescriptorTable.pDescriptorRanges = &descriptor_ranges[i];
        root_parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    root_signature_desc.NumParameters = D3D12_MAX_ROOT_COST;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);
    ID3D12RootSignature_Release(root_signature);

    root_signature_desc.NumParameters = D3D12_MAX_ROOT_COST + 1;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_pipeline_state(void)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_state_desc;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;
    ID3D12Device *device, *tmp_device;
    ULONG refcount;
    HRESULT hr;

    static const DWORD dxbc_code[] =
    {
#if 0
        [numthreads(1, 1, 1)]
        void main() { }
#endif
        0x43425844, 0x1acc3ad0, 0x71c7b057, 0xc72c4306, 0xf432cb57, 0x00000001, 0x00000074, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000020, 0x00050050, 0x00000008, 0x0100086a,
        0x0400009b, 0x00000001, 0x00000001, 0x00000001, 0x0100003e,
    };

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    root_signature_desc.NumParameters = 0;
    root_signature_desc.pParameters = NULL;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    memset(&pipeline_state_desc, 0, sizeof(pipeline_state_desc));
    pipeline_state_desc.pRootSignature = root_signature;
    pipeline_state_desc.CS = shader_bytecode(dxbc_code, sizeof(dxbc_code));
    pipeline_state_desc.NodeMask = 0;
    pipeline_state_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = ID3D12Device_CreateComputePipelineState(device, &pipeline_state_desc,
            &IID_ID3D12PipelineState, (void **)&pipeline_state);
    ok(SUCCEEDED(hr), "CreateComputePipelineState failed, hr %#x.\n", hr);

    refcount = get_refcount(root_signature);
    ok(refcount == 1, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12PipelineState_GetDevice(pipeline_state, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 4, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(pipeline_state, &IID_ID3D12Object, TRUE);
    check_interface(pipeline_state, &IID_ID3D12DeviceChild, TRUE);
    check_interface(pipeline_state, &IID_ID3D12Pageable, TRUE);
    check_interface(pipeline_state, &IID_ID3D12PipelineState, TRUE);

    refcount = ID3D12PipelineState_Release(pipeline_state);
    ok(!refcount, "ID3D12PipelineState has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12RootSignature_Release(root_signature);
    ok(!refcount, "ID3D12RootSignature has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_fence(void)
{
    ID3D12Device *device, *tmp_device;
    ID3D12Fence *fence;
    ULONG refcount;
    UINT64 value;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12Fence_GetDevice(fence, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(fence, &IID_ID3D12Object, TRUE);
    check_interface(fence, &IID_ID3D12DeviceChild, TRUE);
    check_interface(fence, &IID_ID3D12Pageable, TRUE);
    check_interface(fence, &IID_ID3D12Fence, TRUE);

    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    refcount = ID3D12Fence_Release(fence);
    ok(!refcount, "ID3D12Fence has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateFence(device, 99, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 99, "Got unexpected value %"PRIu64".\n", value);
    refcount = ID3D12Fence_Release(fence);
    ok(!refcount, "ID3D12Fence has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_reset_command_allocator(void)
{
    ID3D12CommandAllocator *command_allocator, *command_allocator2;
    ID3D12GraphicsCommandList *command_list, *command_list2;
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == E_FAIL, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == E_FAIL, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting Command list failed, hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == E_FAIL, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator2);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    uav_barrier(command_list, NULL);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    exec_command_list(queue, command_list);

    /* A command list can be reset when it is in use. */
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator2, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    wait_queue_idle(device, queue);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);

    uav_barrier(command_list, NULL);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    exec_command_list(queue, command_list);

    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    wait_queue_idle(device, queue);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);

    /* A command allocator can be used with one command list at a time. */
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list2);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator2, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list2);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12GraphicsCommandList_Close(command_list2);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list2, command_allocator, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandAllocator_Release(command_allocator2);
    ID3D12CommandQueue_Release(queue);
    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12GraphicsCommandList_Release(command_list2);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_cpu_signal_fence(void)
{
    HANDLE event1, event2;
    ID3D12Device *device;
    unsigned int i, ret;
    ID3D12Fence *fence;
    ULONG refcount;
    UINT64 value;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    hr = ID3D12Fence_Signal(fence, 1);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 1, "Got unexpected value %"PRIu64".\n", value);

    hr = ID3D12Fence_Signal(fence, 10);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 10, "Got unexpected value %"PRIu64".\n", value);

    hr = ID3D12Fence_Signal(fence, 5);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 5, "Got unexpected value %"PRIu64".\n", value);

    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    /* Basic tests with single event. */
    event1 = create_event();
    ok(!!event1, "Failed to create event.\n");
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 5, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, 5);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 6, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, 7);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 10);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Event is signaled immediately when value <= GetCompletedValue(). */
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    for (i = 0; i <= ID3D12Fence_GetCompletedValue(fence); ++i)
    {
        hr = ID3D12Fence_SetEventOnCompletion(fence, i, event1);
        ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
        ret = wait_event(event1, 0);
        ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x for %u.\n", ret, i);
        ret = wait_event(event1, 0);
        ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x for %u.\n", ret, i);
    }
    hr = ID3D12Fence_SetEventOnCompletion(fence, i, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, i);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Attach event to multiple values. */
    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 3, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 5, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 9, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 12, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 12, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    for (i = 1; i < 13; ++i)
    {
        hr = ID3D12Fence_Signal(fence, i);
        ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
        if (i == 3 || i == 5 || i == 9 || i == 12)
        {
            ret = wait_event(event1, 0);
            ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x for %u.\n", ret, i);
        }
        ret = wait_event(event1, 0);
        ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x for %u.\n", ret, i);
    }

    /* Tests with 2 events. */
    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    event2 = create_event();
    ok(!!event2, "Failed to create event.\n");

    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 100, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, ~(UINT64)0, event2);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);

    hr = ID3D12Fence_Signal(fence, 50);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 99);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 100);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 101);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 100);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, ~(UINT64)0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, ~(UINT64)0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Attach two events to the same value. */
    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event2);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, 3);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Test passing signaled event. */
    hr = ID3D12Fence_Signal(fence, 20);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 20, "Got unexpected value %"PRIu64".\n", value);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    signal_event(event1);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 30, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 30);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    destroy_event(event1);
    destroy_event(event2);

    ID3D12Fence_Release(fence);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_gpu_signal_fence(void)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandQueue *queue;
    HANDLE event1, event2;
    ID3D12Device *device;
    unsigned int i, ret;
    ID3D12Fence *fence;
    ULONG refcount;
    UINT64 value;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    /* XXX: It seems that when a queue is idle a fence is signalled immediately
     * in D3D12. Vulkan implementations don't signal a fence immediately so
     * libvkd3d doesn't as well. In order to make this test reliable
     * wait_queue_idle() is inserted after every ID3D12CommandQueue_Signal(). */
    hr = ID3D12CommandQueue_Signal(queue, fence, 10);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 10, "Got unexpected value %"PRIu64".\n", value);

    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    /* Basic tests with single event. */
    event1 = create_event();
    ok(!!event1, "Failed to create event.\n");
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 5, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12CommandQueue_Signal(queue, fence, 5);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 6, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12CommandQueue_Signal(queue, fence, 7);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 10);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Attach one event to multiple values. */
    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 3, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 5, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 9, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 12, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 12, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    for (i = 1; i < 13; ++i)
    {
        hr = ID3D12CommandQueue_Signal(queue, fence, i);
        ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
        wait_queue_idle(device, queue);
        if (i == 3 || i == 5 || i == 9 || i == 12)
        {
            ret = wait_event(event1, 0);
            ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x for %u.\n", ret, i);
        }
        ret = wait_event(event1, 0);
        ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x for %u.\n", ret, i);
    }

    /* Tests with 2 events. */
    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    event2 = create_event();
    ok(!!event2, "Failed to create event.\n");

    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 100, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, ~(UINT64)0, event2);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);

    hr = ID3D12CommandQueue_Signal(queue, fence, 50);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 99);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 100);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 101);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 100);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, ~(UINT64)0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, ~(UINT64)0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Attach two events to the same value. */
    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event2);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12CommandQueue_Signal(queue, fence, 3);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    wait_queue_idle(device, queue);

    destroy_event(event1);
    destroy_event(event2);

    ID3D12Fence_Release(fence);
    ID3D12CommandQueue_Release(queue);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

struct multithread_fence_wait_data
{
    HANDLE event;
    ID3D12Fence *fence;
    UINT64 value;
};

static void fence_event_wait_main(void *untyped_data)
{
    struct multithread_fence_wait_data *data = untyped_data;
    HANDLE event;
    HRESULT hr;
    int ret;

    event = create_event();
    ok(!!event, "Failed to create event.\n");

    hr = ID3D12Fence_SetEventOnCompletion(data->fence, data->value, event);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);

    signal_event(data->event);

    ret = wait_event(event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);

    destroy_event(event);
}

static void fence_busy_wait_main(void *untyped_data)
{
    struct multithread_fence_wait_data *data = untyped_data;

    signal_event(data->event);

    while (ID3D12Fence_GetCompletedValue(data->fence) < data->value)
        ;
}

static void test_multithread_fence_wait(void)
{
    struct multithread_fence_wait_data thread_data;
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    unsigned int ret;
    ULONG refcount;
    HANDLE thread;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    thread_data.event = create_event();
    thread_data.value = 0;
    ok(!!thread_data.event, "Failed to create event.\n");
    hr = ID3D12Device_CreateFence(device, thread_data.value, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&thread_data.fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    /* Signal fence on host. */
    ++thread_data.value;
    thread = create_thread(fence_event_wait_main, &thread_data);
    ok(!!thread, "Failed to create thread.\n");
    ret = wait_event(thread_data.event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Failed to wait for thread start, return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(thread_data.fence, thread_data.value);
    ok(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);

    ok(join_thread(thread), "Failed to join thread.\n");

    ++thread_data.value;
    thread = create_thread(fence_busy_wait_main, &thread_data);
    ok(!!thread, "Failed to create thread.\n");
    ret = wait_event(thread_data.event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Failed to wait for thread start, return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(thread_data.fence, thread_data.value);
    ok(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);

    ok(join_thread(thread), "Failed to join thread.\n");

    /* Signal fence on device. */
    ++thread_data.value;
    thread = create_thread(fence_event_wait_main, &thread_data);
    ok(!!thread, "Failed to create thread.\n");
    ret = wait_event(thread_data.event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Failed to wait for thread start, return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, thread_data.fence, thread_data.value);
    ok(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);

    ok(join_thread(thread), "Failed to join thread.\n");

    ++thread_data.value;
    thread = create_thread(fence_busy_wait_main, &thread_data);
    ok(!!thread, "Failed to create thread.\n");
    ret = wait_event(thread_data.event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Failed to wait for thread start, return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, thread_data.fence, thread_data.value);
    ok(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);

    ok(join_thread(thread), "Failed to join thread.\n");

    destroy_event(thread_data.event);
    ID3D12Fence_Release(thread_data.fence);
    ID3D12CommandQueue_Release(queue);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_clear_depth_stencil_view(void)
{
    ID3D12GraphicsCommandList *command_list;
    struct depth_stencil_resource ds;
    unsigned int dsv_increment_size;
    D3D12_CLEAR_VALUE clear_value;
    struct test_context_desc desc;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    dsv_increment_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    trace("DSV descriptor handle increment size: %u.\n", dsv_increment_size);
    ok(dsv_increment_size, "Got unexpected increment size %#x.\n", dsv_increment_size);

    clear_value.Format = DXGI_FORMAT_D32_FLOAT;
    clear_value.DepthStencil.Depth = 0.5f;
    clear_value.DepthStencil.Stencil = 0x3;
    init_depth_stencil(&ds, device, 32, 32, DXGI_FORMAT_D32_FLOAT, 0, &clear_value);

    ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, ds.dsv_handle,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.75f, 0x7, 0, NULL);
    transition_resource_state(command_list, ds.texture,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(ds.texture, 0, queue, command_list, 0x3f400000, 0);

    destroy_depth_stencil(&ds);
    destroy_test_context(&context);
}

static void test_clear_render_target_view(void)
{
    static const float color[] = {0.1f, 0.5f, 0.3f, 0.75f};
    static const float green[] = {0.0f, 1.0f, 0.0f, 1.0f};
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    unsigned int rtv_increment_size;
    ID3D12DescriptorHeap *rtv_heap;
    D3D12_CLEAR_VALUE clear_value;
    struct test_context_desc desc;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    ID3D12Device *device;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &rtv_heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    rtv_increment_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    trace("RTV descriptor handle increment size: %u.\n", rtv_increment_size);

    rtv_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear_value.Color[0] = 1.0f;
    clear_value.Color[1] = 0.0f;
    clear_value.Color[2] = 0.0f;
    clear_value.Color[3] = 1.0f;
    hr = ID3D12Device_CreateCommittedResource(device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "Failed to create texture, hr %#x.\n", hr);

    memset(&rtv_desc, 0, sizeof(rtv_desc));
    rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateRenderTargetView(device, resource, &rtv_desc, rtv_handle);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handle, green, 0, NULL);
    transition_resource_state(command_list, resource,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(resource, 0, queue, command_list, 0xff00ff00, 0);

    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, resource,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handle, color, 0, NULL);
    transition_resource_state(command_list, resource,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(resource, 0, queue, command_list, 0xbf4c7f19, 2);

    /* sRGB view */
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, resource,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ID3D12Device_CreateRenderTargetView(device, resource, &rtv_desc, rtv_handle);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handle, color, 0, NULL);
    transition_resource_state(command_list, resource,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(resource, 0, queue, command_list, 0xbf95bc59, 2);

    ID3D12Resource_Release(resource);
    ID3D12DescriptorHeap_Release(rtv_heap);
    destroy_test_context(&context);
}

static void test_clear_unordered_access_view(void)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    ID3D12DescriptorHeap *cpu_heap, *gpu_heap;
    ID3D12GraphicsCommandList *command_list;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    struct test_context_desc desc;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    ID3D12Resource *buffer;
    ID3D12Device *device;
    UINT clear_value[4];
    unsigned int i, j;
    HRESULT hr;
    RECT rect;

    static const size_t buffer_size = 1024 * 1024;
    static const struct
    {
        DXGI_FORMAT format;
        D3D12_BUFFER_UAV buffer_uav;
        unsigned int values[4];
    }
    tests[] =
    {
        {DXGI_FORMAT_R32_UINT, { 0, buffer_size / sizeof(uint32_t),      0, 0, D3D12_BUFFER_UAV_FLAG_NONE},
                {0, 0, 0, 0}},
        {DXGI_FORMAT_R32_UINT, {64, buffer_size / sizeof(uint32_t) - 64, 0, 0, D3D12_BUFFER_UAV_FLAG_NONE},
                {0, 0, 0, 0}},
        {DXGI_FORMAT_R32_UINT, { 0, buffer_size / sizeof(uint32_t),      0, 0, D3D12_BUFFER_UAV_FLAG_NONE},
                {1, 0, 0, 0}},
        {DXGI_FORMAT_R32_UINT, {64, buffer_size / sizeof(uint32_t) - 64, 0, 0, D3D12_BUFFER_UAV_FLAG_NONE},
                {2, 0, 0, 0}},
        {DXGI_FORMAT_R32_UINT, {64, buffer_size / sizeof(uint32_t) - 64, 0, 0, D3D12_BUFFER_UAV_FLAG_NONE},
                {3, 0, 0, 0}},
        {DXGI_FORMAT_R32_UINT, {64, buffer_size / sizeof(uint32_t) - 64, 0, 0, D3D12_BUFFER_UAV_FLAG_NONE},
                {4, 2, 3, 4}},
        {DXGI_FORMAT_R32_UINT, { 0, buffer_size / sizeof(uint32_t) - 10, 0, 0, D3D12_BUFFER_UAV_FLAG_NONE},
                {5, 0, 0, 0}},

        {DXGI_FORMAT_R32_TYPELESS, { 0, buffer_size / sizeof(uint32_t),      0, 0, D3D12_BUFFER_UAV_FLAG_RAW},
                {0, 0, 0, 0}},
        {DXGI_FORMAT_R32_TYPELESS, {64, buffer_size / sizeof(uint32_t) - 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW},
                {0, 0, 0, 0}},
        {DXGI_FORMAT_R32_TYPELESS, { 0, buffer_size / sizeof(uint32_t),      0, 0, D3D12_BUFFER_UAV_FLAG_RAW},
                {6, 0, 0, 0}},
        {DXGI_FORMAT_R32_TYPELESS, {64, buffer_size / sizeof(uint32_t) - 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW},
                {7, 0, 0, 0}},
        {DXGI_FORMAT_R32_TYPELESS, {64, buffer_size / sizeof(uint32_t) - 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW},
                {8, 0, 0, 0}},
        {DXGI_FORMAT_R32_TYPELESS, {64, buffer_size / sizeof(uint32_t) - 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW},
                {9, 1, 1, 1}},
        {DXGI_FORMAT_R32_TYPELESS, {64, buffer_size / sizeof(uint32_t) - 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW},
                {~0u, 0, 0, 0}},
        {DXGI_FORMAT_R32_TYPELESS, { 0, buffer_size / sizeof(uint32_t) - 10, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW},
                {10, 0, 0, 0}},
        {DXGI_FORMAT_R32_TYPELESS, { 0, buffer_size / sizeof(uint32_t) - 9,  0, 0, D3D12_BUFFER_UAV_FLAG_RAW},
                {11, 0, 0, 0}},
    };

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    heap_desc.NumDescriptors = 2;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cpu_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&gpu_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        buffer = create_default_buffer(device, buffer_size,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        for (j = 0; j < ARRAY_SIZE(clear_value); ++j)
            clear_value[j] = tests[i].values[j] ? 0 : ~0u;

        memset(&uav_desc, 0, sizeof(uav_desc));
        uav_desc.Format = DXGI_FORMAT_R32_UINT;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.NumElements = buffer_size / sizeof(uint32_t);
        ID3D12Device_CreateUnorderedAccessView(device, buffer, NULL, &uav_desc,
                get_cpu_descriptor_handle(&context, cpu_heap, 1));
        ID3D12Device_CreateUnorderedAccessView(device, buffer, NULL, &uav_desc,
                get_cpu_descriptor_handle(&context, gpu_heap, 1));

        uav_desc.Format = tests[i].format;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer = tests[i].buffer_uav;
        ID3D12Device_CreateUnorderedAccessView(device, buffer, NULL, &uav_desc,
                get_cpu_descriptor_handle(&context, cpu_heap, 0));
        ID3D12Device_CreateUnorderedAccessView(device, buffer, NULL, &uav_desc,
                get_cpu_descriptor_handle(&context, gpu_heap, 0));

        ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(command_list,
                get_gpu_descriptor_handle(&context, gpu_heap, 1),
                get_cpu_descriptor_handle(&context, cpu_heap, 1),
                buffer, clear_value, 0, NULL);
        ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(command_list,
                get_gpu_descriptor_handle(&context, gpu_heap, 0),
                get_cpu_descriptor_handle(&context, cpu_heap, 0),
                buffer, tests[i].values, 0, NULL);

        rect.top = 0;
        rect.bottom = 1;
        transition_resource_state(command_list, buffer,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        get_buffer_readback_with_command_list(buffer, DXGI_FORMAT_R32_TYPELESS, &rb, queue, command_list);
        rect.left = 0;
        rect.right = uav_desc.Buffer.FirstElement;
        check_readback_data_uint(&rb, &rect, clear_value[0], 0);
        rect.left = uav_desc.Buffer.FirstElement;
        rect.right = uav_desc.Buffer.FirstElement + uav_desc.Buffer.NumElements;
        check_readback_data_uint(&rb, &rect, tests[i].values[0], 0);
        rect.left = uav_desc.Buffer.FirstElement + uav_desc.Buffer.NumElements;
        rect.right = buffer_size / format_size(uav_desc.Format);
        check_readback_data_uint(&rb, &rect, clear_value[0], 0);
        release_resource_readback(&rb);

        reset_command_list(command_list, context.allocator);
        ID3D12Resource_Release(buffer);
    }

    ID3D12DescriptorHeap_Release(cpu_heap);
    ID3D12DescriptorHeap_Release(gpu_heap);
    destroy_test_context(&context);
}

static void test_draw_instanced(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    struct test_context context;
    ID3D12CommandQueue *queue;

    if (!init_test_context(&context, NULL))
        return;
    command_list = context.list;
    queue = context.queue;

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    if (!use_warp_device)
    {
        /* This draw call is ignored. */
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);
    }

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    check_sub_resource_uint(context.render_target, 0, queue, command_list, 0xff00ff00, 0);

    destroy_test_context(&context);
}

static void test_draw_indexed_instanced(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const uint16_t indices[] = {0, 1, 2};
    ID3D12GraphicsCommandList *command_list;
    struct test_context context;
    D3D12_INDEX_BUFFER_VIEW ibv;
    ID3D12CommandQueue *queue;
    ID3D12Resource *ib;

    if (!init_test_context(&context, NULL))
        return;
    command_list = context.list;
    queue = context.queue;

    ib = create_upload_buffer(context.device, sizeof(indices), indices);

    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ib);
    ibv.SizeInBytes = sizeof(indices);
    ibv.Format = DXGI_FORMAT_R16_UINT;

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    if (!use_warp_device)
    {
        /* This draw call is ignored. */
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 3, 1, 0, 0, 0);
    }

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 3, 1, 0, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    check_sub_resource_uint(context.render_target, 0, queue, command_list, 0xff00ff00, 0);

    ID3D12Resource_Release(ib);
    destroy_test_context(&context);
}

static void test_append_aligned_element(void)
{
    ID3D12GraphicsCommandList *command_list;
    D3D12_INPUT_LAYOUT_DESC input_layout;
    D3D12_VERTEX_BUFFER_VIEW vbv[3];
    struct test_context_desc desc;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    ID3D12Resource *vb[3];
    unsigned int color;

    /* Semantic names are case-insensitive. */
    static const D3D12_INPUT_ELEMENT_DESC layout_desc[] =
    {
        {"CoLoR",    2, DXGI_FORMAT_R32G32_FLOAT,       1, D3D12_APPEND_ALIGNED_ELEMENT,
                D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"ColoR",    3, DXGI_FORMAT_R32G32_FLOAT,       2, D3D12_APPEND_ALIGNED_ELEMENT,
                D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"ColoR",    0, DXGI_FORMAT_R32G32_FLOAT,       2, D3D12_APPEND_ALIGNED_ELEMENT,
                D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"cOLOr",    1, DXGI_FORMAT_R32G32_FLOAT,       1, D3D12_APPEND_ALIGNED_ELEMENT,
                D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    static const DWORD vs_code[] =
    {
#if 0
        struct vs_in
        {
            float4 position : POSITION;
            float2 color_xy : COLOR0;
            float2 color_zw : COLOR1;
            unsigned int instance_id : SV_INSTANCEID;
        };

        struct vs_out
        {
            float4 position : SV_POSITION;
            float2 color_xy : COLOR0;
            float2 color_zw : COLOR1;
        };

        struct vs_out main(struct vs_in i)
        {
            struct vs_out o;

            o.position = i.position;
            o.position.x += i.instance_id * 0.5;
            o.color_xy = i.color_xy;
            o.color_zw = i.color_zw;

            return o;
        }
#endif
        0x43425844, 0x52e3bf46, 0x6300403d, 0x624cffe4, 0xa4fc0013, 0x00000001, 0x00000214, 0x00000003,
        0x0000002c, 0x000000bc, 0x00000128, 0x4e475349, 0x00000088, 0x00000004, 0x00000008, 0x00000068,
        0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x00000f0f, 0x00000071, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000303, 0x00000071, 0x00000001, 0x00000000, 0x00000003, 0x00000002,
        0x00000303, 0x00000077, 0x00000000, 0x00000008, 0x00000001, 0x00000003, 0x00000101, 0x49534f50,
        0x4e4f4954, 0x4c4f4300, 0x5300524f, 0x4e495f56, 0x4e415453, 0x44494543, 0xababab00, 0x4e47534f,
        0x00000064, 0x00000003, 0x00000008, 0x00000050, 0x00000000, 0x00000001, 0x00000003, 0x00000000,
        0x0000000f, 0x0000005c, 0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x00000c03, 0x0000005c,
        0x00000001, 0x00000000, 0x00000003, 0x00000001, 0x0000030c, 0x505f5653, 0x5449534f, 0x004e4f49,
        0x4f4c4f43, 0xabab0052, 0x52444853, 0x000000e4, 0x00010040, 0x00000039, 0x0300005f, 0x001010f2,
        0x00000000, 0x0300005f, 0x00101032, 0x00000001, 0x0300005f, 0x00101032, 0x00000002, 0x04000060,
        0x00101012, 0x00000003, 0x00000008, 0x04000067, 0x001020f2, 0x00000000, 0x00000001, 0x03000065,
        0x00102032, 0x00000001, 0x03000065, 0x001020c2, 0x00000001, 0x02000068, 0x00000001, 0x05000056,
        0x00100012, 0x00000000, 0x0010100a, 0x00000003, 0x09000032, 0x00102012, 0x00000000, 0x0010000a,
        0x00000000, 0x00004001, 0x3f000000, 0x0010100a, 0x00000000, 0x05000036, 0x001020e2, 0x00000000,
        0x00101e56, 0x00000000, 0x05000036, 0x00102032, 0x00000001, 0x00101046, 0x00000001, 0x05000036,
        0x001020c2, 0x00000001, 0x00101406, 0x00000002, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs = {vs_code, sizeof(vs_code)};
    static const DWORD ps_code[] =
    {
#if 0
        struct vs_out
        {
            float4 position : SV_POSITION;
            float2 color_xy : COLOR0;
            float2 color_zw : COLOR1;
        };

        float4 main(struct vs_out i) : SV_TARGET
        {
            return float4(i.color_xy.xy, i.color_zw.xy);
        }
#endif
        0x43425844, 0x64e48a09, 0xaa484d46, 0xe40a6e78, 0x9885edf3, 0x00000001, 0x00000118, 0x00000003,
        0x0000002c, 0x00000098, 0x000000cc, 0x4e475349, 0x00000064, 0x00000003, 0x00000008, 0x00000050,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000303, 0x0000005c, 0x00000001, 0x00000000, 0x00000003, 0x00000001,
        0x00000c0c, 0x505f5653, 0x5449534f, 0x004e4f49, 0x4f4c4f43, 0xabab0052, 0x4e47534f, 0x0000002c,
        0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x0000000f,
        0x545f5653, 0x45475241, 0xabab0054, 0x52444853, 0x00000044, 0x00000040, 0x00000011, 0x03001062,
        0x00101032, 0x00000001, 0x03001062, 0x001010c2, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const struct
    {
        struct vec4 position;
    }
    stream0[] =
    {
        {{-1.0f, -1.0f, 0.0f, 1.0f}},
        {{-1.0f,  1.0f, 0.0f, 1.0f}},
        {{-0.5f, -1.0f, 0.0f, 1.0f}},
        {{-0.5f,  1.0f, 0.0f, 1.0f}},
    };
    static const struct
    {
        struct vec2 color2;
        struct vec2 color1;
    }
    stream1[] =
    {
        {{0.5f, 0.5f}, {0.0f, 1.0f}},
        {{0.5f, 0.5f}, {0.0f, 1.0f}},
        {{0.5f, 0.5f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f}, {1.0f, 1.0f}},
    };
    static const struct
    {
        struct vec2 color3;
        struct vec2 color0;
    }
    stream2[] =
    {
        {{0.5f, 0.5f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f}, {0.0f, 1.0f}},
        {{0.5f, 0.5f}, {0.0f, 0.0f}},
        {{0.5f, 0.5f}, {1.0f, 0.0f}},
    };
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};

    memset(&desc, 0, sizeof(desc));
    desc.rt_width = 640;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_empty_root_signature(context.device,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    input_layout.pInputElementDescs = layout_desc;
    input_layout.NumElements = ARRAY_SIZE(layout_desc);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, context.render_target_desc.Format, &vs, &ps, &input_layout);

    vb[0] = create_upload_buffer(context.device, sizeof(stream0), stream0);
    vbv[0].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb[0]);
    vbv[0].StrideInBytes = sizeof(*stream0);
    vbv[0].SizeInBytes = sizeof(stream0);

    vb[1] = create_upload_buffer(context.device, sizeof(stream1), stream1);
    vbv[1].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb[1]);
    vbv[1].StrideInBytes = sizeof(*stream1);
    vbv[1].SizeInBytes = sizeof(stream1);

    vb[2] = create_upload_buffer(context.device, sizeof(stream2), stream2);
    vbv[2].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb[2]);
    vbv[2].StrideInBytes = sizeof(*stream2);
    vbv[2].SizeInBytes = sizeof(stream2);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, ARRAY_SIZE(vbv), vbv);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 4, 4, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    color = get_readback_uint(&rb, 80, 16);
    ok(compare_color(color, 0xff0000ff, 1), "Got unexpected color 0x%08x.\n", color);
    color = get_readback_uint(&rb, 240, 16);
    ok(compare_color(color, 0xff00ff00, 1), "Got unexpected color 0x%08x.\n", color);
    color = get_readback_uint(&rb, 400, 16);
    ok(compare_color(color, 0xffff0000, 1), "Got unexpected color 0x%08x.\n", color);
    color = get_readback_uint(&rb, 560, 16);
    ok(compare_color(color, 0xffff00ff, 1), "Got unexpected color 0x%08x.\n", color);
    release_resource_readback(&rb);

    ID3D12Resource_Release(vb[2]);
    ID3D12Resource_Release(vb[1]);
    ID3D12Resource_Release(vb[0]);
    destroy_test_context(&context);
}

static void test_gpu_virtual_address(void)
{
    D3D12_GPU_VIRTUAL_ADDRESS vb_offset, ib_offset;
    ID3D12GraphicsCommandList *command_list;
    D3D12_INPUT_LAYOUT_DESC input_layout;
    struct test_context_desc desc;
    D3D12_VERTEX_BUFFER_VIEW vbv;
    struct test_context context;
    D3D12_INDEX_BUFFER_VIEW ibv;
    ID3D12CommandQueue *queue;
    ID3D12Resource *buffer;
    HRESULT hr;
    BYTE *ptr;

    static const DWORD vs_code[] =
    {
#if 0
        void main(float4 in_position : POSITION, float4 in_color : COLOR,
                out float4 out_position : SV_POSITION, out float4 out_color : COLOR)
        {
            out_position = in_position;
            out_color = in_color;
        }
#endif
        0x43425844, 0xa58fc911, 0x280038e9, 0x14cfff54, 0xe43fc328, 0x00000001, 0x00000144, 0x00000003,
        0x0000002c, 0x0000007c, 0x000000d0, 0x4e475349, 0x00000048, 0x00000002, 0x00000008, 0x00000038,
        0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x00000f0f, 0x00000041, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000f0f, 0x49534f50, 0x4e4f4954, 0x4c4f4300, 0xab00524f, 0x4e47534f,
        0x0000004c, 0x00000002, 0x00000008, 0x00000038, 0x00000000, 0x00000001, 0x00000003, 0x00000000,
        0x0000000f, 0x00000044, 0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x0000000f, 0x505f5653,
        0x5449534f, 0x004e4f49, 0x4f4c4f43, 0xabab0052, 0x58454853, 0x0000006c, 0x00010050, 0x0000001b,
        0x0100086a, 0x0300005f, 0x001010f2, 0x00000000, 0x0300005f, 0x001010f2, 0x00000001, 0x04000067,
        0x001020f2, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000001, 0x05000036, 0x001020f2,
        0x00000000, 0x00101e46, 0x00000000, 0x05000036, 0x001020f2, 0x00000001, 0x00101e46, 0x00000001,
        0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs = {vs_code, sizeof(vs_code)};
    static const DWORD ps_code[] =
    {
#if 0
        void main(float4 in_position : SV_POSITION, float4 in_color : COLOR,
                out float4 out_color : SV_TARGET)
        {
            out_color = in_color;
        }
#endif
        0x43425844, 0x1a6def50, 0x9c069300, 0x7cce68f0, 0x621239b9, 0x00000001, 0x000000f8, 0x00000003,
        0x0000002c, 0x00000080, 0x000000b4, 0x4e475349, 0x0000004c, 0x00000002, 0x00000008, 0x00000038,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x00000044, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000f0f, 0x505f5653, 0x5449534f, 0x004e4f49, 0x4f4c4f43, 0xabab0052,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x45475241, 0xabab0054, 0x58454853, 0x0000003c, 0x00000050,
        0x0000000f, 0x0100086a, 0x03001062, 0x001010f2, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const uint32_t indices[] = {0, 1, 2, 3};
    static const D3D12_INPUT_ELEMENT_DESC layout_desc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static const struct
    {
        struct vec2 position;
        struct vec4 color;
    }
    quad[] =
    {
        {{-1.0f, -1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{-1.0f,  1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{ 1.0f, -1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{ 1.0f,  1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    };

    memset(&desc, 0, sizeof(desc));
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_empty_root_signature(context.device,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    input_layout.pInputElementDescs = layout_desc;
    input_layout.NumElements = ARRAY_SIZE(layout_desc);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, context.render_target_desc.Format, &vs, &ps, &input_layout);

    vb_offset = 0x200;
    ib_offset = 0x500;
    buffer = create_upload_buffer(context.device, ib_offset + sizeof(indices), NULL);

    hr = ID3D12Resource_Map(buffer, 0, NULL, (void **)&ptr);
    ok(SUCCEEDED(hr), "Failed to map upload buffer, hr %#x.\n", hr);
    memcpy(ptr + vb_offset, quad, sizeof(quad));
    memcpy(ptr + ib_offset, indices, sizeof(indices));
    ID3D12Resource_Unmap(buffer, 0, NULL);

    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buffer) + vb_offset;
    vbv.StrideInBytes = sizeof(*quad);
    vbv.SizeInBytes = sizeof(quad);
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buffer) + ib_offset;
    ibv.SizeInBytes = sizeof(indices);
    ibv.Format = DXGI_FORMAT_R32_UINT;

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 4, 1, 0, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target, 0, queue, command_list, 0xff00ff00, 0);

    ID3D12Resource_Release(buffer);
    destroy_test_context(&context);
}

static void test_fragment_coords(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    struct test_context_desc desc;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    unsigned int x, y;

    static const DWORD ps_code[] =
    {
#if 0
        float4 main(float4 position: sv_position) : sv_target
        {
            return position;
        }
#endif
        0x43425844, 0xac408178, 0x2ca4213f, 0x4f2551e1, 0x1626b422, 0x00000001, 0x000000d8, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x00000f0f, 0x705f7673, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x745f7673, 0x65677261, 0xabab0074, 0x52444853, 0x0000003c, 0x00000040,
        0x0000000f, 0x04002064, 0x001010f2, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_pipeline = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, NULL, &ps, NULL);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    set_viewport(&context.viewport, 10.0f, 10.0f, 20.0f, 30.0f, 0.0f, 1.0f);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < rb.height; ++y)
    {
        for (x = 0; x < rb.width; ++x)
        {
            const struct vec4 *v = get_readback_vec4(&rb, x, y);
            struct vec4 expected = {x + 0.5f, y + 0.5f, 0.0f, 1.0f};
            ok(compare_vec4(v, &expected, 0),
                    "Got %.8e, %.8e, %.8e, %.8e expected %.8e, %.8e, %.8e, %.8e.\n",
                    v->x, v->y, v->z, v->w, expected.x, expected.y, expected.z, expected.w);
        }
    }
    release_resource_readback(&rb);

    destroy_test_context(&context);
}

static void test_fractional_viewports(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    D3D12_INPUT_LAYOUT_DESC input_layout;
    struct test_context_desc desc;
    D3D12_VERTEX_BUFFER_VIEW vbv;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    D3D12_VIEWPORT viewport;
    unsigned int i, x, y;
    ID3D12Resource *vb;

    static const DWORD vs_code[] =
    {
#if 0
        void main(in float4 in_position : POSITION,
                in float2 in_texcoord : TEXCOORD,
                out float4 position : SV_Position,
                out float2 texcoord : TEXCOORD)
        {
            position = in_position;
            texcoord = in_texcoord;
        }
#endif
        0x43425844, 0x4df282ca, 0x85c8bbfc, 0xd44ad19f, 0x1158be97, 0x00000001, 0x00000148, 0x00000003,
        0x0000002c, 0x00000080, 0x000000d8, 0x4e475349, 0x0000004c, 0x00000002, 0x00000008, 0x00000038,
        0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x00000f0f, 0x00000041, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000303, 0x49534f50, 0x4e4f4954, 0x58455400, 0x524f4f43, 0xabab0044,
        0x4e47534f, 0x00000050, 0x00000002, 0x00000008, 0x00000038, 0x00000000, 0x00000001, 0x00000003,
        0x00000000, 0x0000000f, 0x00000044, 0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x00000c03,
        0x505f5653, 0x7469736f, 0x006e6f69, 0x43584554, 0x44524f4f, 0xababab00, 0x52444853, 0x00000068,
        0x00010040, 0x0000001a, 0x0300005f, 0x001010f2, 0x00000000, 0x0300005f, 0x00101032, 0x00000001,
        0x04000067, 0x001020f2, 0x00000000, 0x00000001, 0x03000065, 0x00102032, 0x00000001, 0x05000036,
        0x001020f2, 0x00000000, 0x00101e46, 0x00000000, 0x05000036, 0x00102032, 0x00000001, 0x00101046,
        0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs = {vs_code, sizeof(vs_code)};
    static const DWORD ps_code[] =
    {
#if 0
        float4 main(float4 position : SV_Position,
                float2 texcoord : TEXCOORD) : SV_Target
        {
            return float4(position.xy, texcoord);
        }
#endif
        0x43425844, 0xa15616bc, 0x6862ab1c, 0x28b915c0, 0xdb0df67c, 0x00000001, 0x0000011c, 0x00000003,
        0x0000002c, 0x00000084, 0x000000b8, 0x4e475349, 0x00000050, 0x00000002, 0x00000008, 0x00000038,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x00000044, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000303, 0x505f5653, 0x7469736f, 0x006e6f69, 0x43584554, 0x44524f4f,
        0xababab00, 0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000,
        0x00000003, 0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x0000005c,
        0x00000040, 0x00000017, 0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03001062, 0x00101032,
        0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x05000036, 0x00102032, 0x00000000, 0x00101046,
        0x00000000, 0x05000036, 0x001020c2, 0x00000000, 0x00101406, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const D3D12_INPUT_ELEMENT_DESC layout_desc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static const struct
    {
        struct vec2 position;
        struct vec2 texcoord;
    }
    quad[] =
    {
        {{-1.0f, -1.0f}, {0.0f, 0.0f}},
        {{-1.0f,  1.0f}, {0.0f, 1.0f}},
        {{ 1.0f, -1.0f}, {1.0f, 0.0f}},
        {{ 1.0f,  1.0f}, {1.0f, 1.0f}},
    };
    static const float viewport_offsets[] =
    {
        0.0f, 1.0f / 2.0f, 1.0f / 4.0f, 1.0f / 8.0f, 1.0f / 16.0f, 1.0f / 32.0f,
        1.0f / 64.0f, 1.0f / 128.0f, 1.0f / 256.0f, 63.0f / 128.0f,
    };

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_empty_root_signature(context.device,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    input_layout.pInputElementDescs = layout_desc;
    input_layout.NumElements = ARRAY_SIZE(layout_desc);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, &vs, &ps, &input_layout);

    vb = create_upload_buffer(context.device, sizeof(quad), quad);

    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb);
    vbv.StrideInBytes = sizeof(*quad);
    vbv.SizeInBytes = sizeof(quad);

    for (i = 0; i < ARRAY_SIZE(viewport_offsets); ++i)
    {
        set_viewport(&viewport, viewport_offsets[i], viewport_offsets[i],
                context.render_target_desc.Width, context.render_target_desc.Height, 0.0f, 1.0f);

        if (i)
            transition_resource_state(command_list, context.render_target,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 4, 1, 0, 0);

        transition_resource_state(command_list, context.render_target,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
        for (y = 0; y < rb.height; ++y)
        {
            for (x = 0; x < rb.width; ++x)
            {
                const struct vec4 *v = get_readback_vec4(&rb, x, y);
                struct vec4 expected = {x + 0.5f, y + 0.5f,
                        (x + 0.5f - viewport_offsets[i]) / context.render_target_desc.Width,
                        1.0f - (y + 0.5f - viewport_offsets[i]) / context.render_target_desc.Height};
                ok(compare_float(v->x, expected.x, 0) && compare_float(v->y, expected.y, 0),
                        "Got fragcoord {%.8e, %.8e}, expected {%.8e, %.8e} at (%u, %u), offset %.8e.\n",
                        v->x, v->y, expected.x, expected.y, x, y, viewport_offsets[i]);
                ok(compare_float(v->z, expected.z, 2) && compare_float(v->w, expected.w, 2),
                        "Got texcoord {%.8e, %.8e}, expected {%.8e, %.8e} at (%u, %u), offset %.8e.\n",
                        v->z, v->w, expected.z, expected.w, x, y, viewport_offsets[i]);
            }
        }
        release_resource_readback(&rb);

        reset_command_list(command_list, context.allocator);
    }

    ID3D12Resource_Release(vb);
    destroy_test_context(&context);
}

static void test_scissor(void)
{
    ID3D12GraphicsCommandList *command_list;
    struct test_context_desc desc;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    unsigned int color;
    RECT scissor_rect;

    static const DWORD ps_code[] =
    {
#if 0
        float4 main(float4 position : SV_POSITION) : SV_Target
        {
            return float4(0.0, 1.0, 0.0, 1.0);
        }
#endif
        0x43425844, 0x30240e72, 0x012f250c, 0x8673c6ea, 0x392e4cec, 0x00000001, 0x000000d4, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x505f5653, 0x5449534f, 0x004e4f49,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x00000038, 0x00000040,
        0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x08000036, 0x001020f2, 0x00000000, 0x00004002,
        0x00000000, 0x3f800000, 0x00000000, 0x3f800000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const float red[] = {1.0f, 0.0f, 0.0f, 1.0f};

    memset(&desc, 0, sizeof(desc));
    desc.rt_width = 640;
    desc.rt_height = 480;
    desc.no_pipeline = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps, NULL);

    set_rect(&scissor_rect, 160, 120, 480, 360);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, red, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    color = get_readback_uint(&rb, 320, 60);
    ok(compare_color(color, 0xff0000ff, 1), "Got unexpected color 0x%08x.\n", color);
    color = get_readback_uint(&rb, 80, 240);
    ok(compare_color(color, 0xff0000ff, 1), "Got unexpected color 0x%08x.\n", color);
    color = get_readback_uint(&rb, 320, 240);
    ok(compare_color(color, 0xff00ff00, 1), "Got unexpected color 0x%08x.\n", color);
    color = get_readback_uint(&rb, 560, 240);
    ok(compare_color(color, 0xff0000ff, 1), "Got unexpected color 0x%08x.\n", color);
    color = get_readback_uint(&rb, 320, 420);
    ok(compare_color(color, 0xff0000ff, 1), "Got unexpected color 0x%08x.\n", color);
    release_resource_readback(&rb);

    destroy_test_context(&context);
}

static void test_draw_depth_only(void)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    ID3D12GraphicsCommandList *command_list;
    struct depth_stencil_resource ds;
    struct test_context_desc desc;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    unsigned int i, j;
    HRESULT hr;

    static const DWORD ps_code[] =
    {
#if 0
        float depth;

        float main() : SV_Depth
        {
            return depth;
        }
#endif
        0x43425844, 0x91af6cd0, 0x7e884502, 0xcede4f54, 0x6f2c9326, 0x00000001, 0x000000b0, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0xffffffff,
        0x00000e01, 0x445f5653, 0x68747065, 0xababab00, 0x52444853, 0x00000038, 0x00000040, 0x0000000e,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x02000065, 0x0000c001, 0x05000036, 0x0000c001,
        0x0020800a, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const struct
    {
        float clear_depth;
        float depth;
        float expected_depth;
    }
    tests[] =
    {
        {0.0f, 0.0f, 0.0f},
        {0.0f, 0.7f, 0.0f},
        {0.0f, 0.8f, 0.0f},
        {0.0f, 0.5f, 0.0f},

        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.7f, 0.7f},
        {1.0f, 0.8f, 0.8f},
        {1.0f, 0.5f, 0.5f},
    };

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    init_depth_stencil(&ds, context.device, 640, 480, DXGI_FORMAT_D32_FLOAT, 0, NULL);
    set_viewport(&context.viewport, 0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    set_rect(&context.scissor_rect, 0, 0, 640, 480);

    context.root_signature = create_32bit_constants_root_signature(context.device,
            0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    init_pipeline_state_desc(&pso_desc, context.root_signature, 0, NULL, &ps, NULL);
    pso_desc.NumRenderTargets = 0;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = ID3D12Device_CreateGraphicsPipelineState(context.device, &pso_desc,
            &IID_ID3D12PipelineState, (void **)&context.pipeline_state);
    ok(SUCCEEDED(hr), "Failed to create graphics pipeline state, hr %#x.\n", hr);

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, ds.dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH, tests[i].clear_depth, 0, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 0, NULL, FALSE, &ds.dsv_handle);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);

        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 1, &tests[i].depth, 0);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        transition_resource_state(command_list, ds.texture,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check_sub_resource_float(ds.texture, 0, queue, command_list, tests[i].expected_depth, 1);

        reset_command_list(command_list, context.allocator);
        transition_resource_state(command_list, ds.texture,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, ds.dsv_handle,
            D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 0, NULL, FALSE, &ds.dsv_handle);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    for (i = 0; i < 4; ++i)
    {
        for (j = 0; j < 4; ++j)
        {
            float depth = 1.0f / 16.0f * (j + 4 * i);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 1, &depth, 0);

            set_viewport(&context.viewport, 160.0f * j, 120.0f * i, 160.0f, 120.0f, 0.0f, 1.0f);
            ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);

            ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);
        }
    }
    transition_resource_state(command_list, ds.texture,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(ds.texture, 0, &rb, queue, command_list);
    for (i = 0; i < 4; ++i)
    {
        for (j = 0; j < 4; ++j)
        {
            float obtained_depth, expected_depth;

            obtained_depth = get_readback_float(&rb, 80 + j * 160, 60 + i * 120);
            expected_depth = 1.0f / 16.0f * (j + 4 * i);
            ok(compare_float(obtained_depth, expected_depth, 1),
                    "Got unexpected depth %.8e at (%u, %u), expected %.8e.\n",
                    obtained_depth, j, i, expected_depth);
        }
    }
    release_resource_readback(&rb);

    destroy_depth_stencil(&ds);
    destroy_test_context(&context);
}

static void test_draw_uav_only(void)
{
    ID3D12DescriptorHeap *cpu_descriptor_heap, *descriptor_heap;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_range;
    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    D3D12_ROOT_PARAMETER root_parameter;
    struct test_context_desc desc;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    unsigned int i;
    HRESULT hr;

    static const DWORD ps_code[] =
    {
#if 0
        RWTexture2D<int> u;

        void main()
        {
            InterlockedAdd(u[uint2(0, 0)], 1);
        }
#endif
        0x43425844, 0x237a8398, 0xe7b34c17, 0xa28c91a4, 0xb3614d73, 0x00000001, 0x0000009c, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000048, 0x00000050, 0x00000012, 0x0100086a,
        0x0400189c, 0x0011e000, 0x00000000, 0x00003333, 0x0a0000ad, 0x0011e000, 0x00000000, 0x00004002,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00004001, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const float zero[4] = {};

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_range.NumDescriptors = 1;
    descriptor_range.BaseShaderRegister = 0;
    descriptor_range.RegisterSpace = 0;
    descriptor_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameter.DescriptorTable.NumDescriptorRanges = 1;
    root_parameter.DescriptorTable.pDescriptorRanges = &descriptor_range;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = &root_parameter;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(context.device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    init_pipeline_state_desc(&pso_desc, context.root_signature, 0, NULL, &ps, NULL);
    pso_desc.NumRenderTargets = 0;
    hr = ID3D12Device_CreateGraphicsPipelineState(context.device, &pso_desc,
            &IID_ID3D12PipelineState, (void **)&context.pipeline_state);
    ok(SUCCEEDED(hr), "Failed to create graphics pipeline state, hr %#x.\n", hr);

    resource = create_default_texture(context.device, 1, 1, DXGI_FORMAT_R32_SINT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    heap_desc.Flags = 0;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cpu_descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap);
    gpu_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap);
    ID3D12Device_CreateUnorderedAccessView(context.device, resource, NULL, NULL, cpu_handle);
    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_descriptor_heap);
    ID3D12Device_CreateUnorderedAccessView(context.device, resource, NULL, NULL, cpu_handle);

    ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(command_list,
            gpu_handle, cpu_handle, resource, zero, 0, NULL);

    set_rect(&context.scissor_rect, 0, 0, 1000, 1000);
    set_viewport(&context.viewport, 0.0f, 0.0f, 1.0f, 100.0f, 0.0f, 0.0f);

    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gpu_handle);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);

    for (i = 0; i < 5; ++i)
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(resource, 0, queue, command_list, 500, 0);

    ID3D12DescriptorHeap_Release(cpu_descriptor_heap);
    ID3D12DescriptorHeap_Release(descriptor_heap);
    ID3D12Resource_Release(resource);
    destroy_test_context(&context);
}

static void test_texture_resource_barriers(void)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandAllocator *command_allocator;
    ID3D12GraphicsCommandList *command_list;
    D3D12_RESOURCE_BARRIER barriers[8];
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    resource = create_default_texture(device, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[0].Transition.pResource = resource;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[0]);

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[1].UAV.pResource = resource;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[1]);

    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[2].Transition.pResource = resource;
    barriers[2].Transition.Subresource = 0;
    barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[2]);

    barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[3].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[3].Transition.pResource = resource;
    barriers[3].Transition.Subresource = 0;
    barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[3]);

    barriers[4].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[4].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[4].Transition.pResource = resource;
    barriers[4].Transition.Subresource = 0;
    barriers[4].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[4].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[4]);

    barriers[5].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[5].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[5].Transition.pResource = resource;
    barriers[5].Transition.Subresource = 0;
    barriers[5].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[5].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[5]);

    barriers[6].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[6].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[6].UAV.pResource = resource;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[6]);
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[6]);

    barriers[7].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[7].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[7].Transition.pResource = resource;
    barriers[7].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[7].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[7].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[7]);

    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 8, barriers);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);

    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12Resource_Release(resource);
    ID3D12CommandQueue_Release(queue);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_invalid_texture_resource_barriers(void)
{
    ID3D12Resource *texture, *readback_buffer, *upload_buffer;
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandAllocator *command_allocator;
    ID3D12GraphicsCommandList *command_list;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    texture = create_default_texture(device, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    upload_buffer = create_upload_buffer(device, 32, NULL);
    readback_buffer = create_readback_buffer(device, 32);

    /* The following invalid barrier is not detected by the runtime. */
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    reset_command_list(command_list, command_allocator);

    /* The before state does not match with the previous state. */
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    /* The returned error code has changed after a Windows update. */
    ok(hr == S_OK || hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    if (hr == S_OK)
    {
        exec_command_list(queue, command_list);
        wait_queue_idle(device, queue);
    }

    ID3D12GraphicsCommandList_Release(command_list);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* The before state does not match with the previous state. */
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    /* The returned error code has changed after a Windows update. */
    ok(hr == S_OK || hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    if (hr == S_OK)
    {
        exec_command_list(queue, command_list);
        wait_queue_idle(device, queue);
    }

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    ID3D12GraphicsCommandList_Release(command_list);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* Exactly one write state or a combination of read-only states are allowed. */
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    ID3D12GraphicsCommandList_Release(command_list);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* Readback resources cannot transition from D3D12_RESOURCE_STATE_COPY_DEST. */
    transition_resource_state(command_list, readback_buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    todo(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    ID3D12GraphicsCommandList_Release(command_list);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* Upload resources cannot transition from D3D12_RESOURCE_STATE_GENERIC_READ. */
    transition_resource_state(command_list, upload_buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    todo(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandQueue_Release(queue);
    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12Resource_Release(readback_buffer);
    ID3D12Resource_Release(texture);
    ID3D12Resource_Release(upload_buffer);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_device_removed_reason(void)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandAllocator *command_allocator;
    ID3D12GraphicsCommandList *command_list;
    ID3D12CommandQueue *queue, *tmp_queue;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_GetDeviceRemovedReason(device);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* Execute a command list in the recording state. */
    exec_command_list(queue, command_list);

    hr = ID3D12Device_GetDeviceRemovedReason(device);
    todo(hr == DXGI_ERROR_INVALID_CALL, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&tmp_queue);
    todo(hr == DXGI_ERROR_DEVICE_REMOVED, "Got unexpected hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ID3D12CommandQueue_Release(tmp_queue);

    hr = ID3D12Device_GetDeviceRemovedReason(device);
    todo(hr == DXGI_ERROR_INVALID_CALL, "Got unexpected hr %#x.\n", hr);

    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandQueue_Release(queue);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_map_resource(void)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Resource *resource;
    ID3D12Device *device;
    ULONG refcount;
    void *data;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = 0;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "Failed to create texture, hr %#x.\n", hr);

    /* Resources on a DEFAULT heap cannot be mapped. */
    hr = ID3D12Resource_Map(resource, 0, NULL, &data);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    ID3D12Resource_Release(resource);

    heap_properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    if (FAILED(hr))
    {
        skip("Failed to create texture on custom heap.\n");
    }
    else
    {
        /* The data pointer must be NULL for the UNKNOWN layout. */
        hr = ID3D12Resource_Map(resource, 0, NULL, &data);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

        ID3D12Resource_Release(resource);
    }

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Height = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    /* Resources on a DEFAULT heap cannot be mapped. */
    hr = ID3D12Resource_Map(resource, 0, NULL, &data);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    ID3D12Resource_Release(resource);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_bundle_state_inheritance(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list, *bundle;
    ID3D12CommandAllocator *bundle_allocator;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    unsigned int x, y;
    HRESULT hr;

#ifndef _WIN32
    /* Avoid 2048 test todos. */
    skip("Bundles are not implemented yet.\n");
    return;
#endif

    if (use_warp_device)
    {
        skip("Bundle state inheritance test crashes on WARP.\n");
        return;
    }

    if (!init_test_context(&context, NULL))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            &IID_ID3D12CommandAllocator, (void **)&bundle_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            bundle_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&bundle);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* A bundle does not inherit the current pipeline state. */
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);

    ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
    hr = ID3D12GraphicsCommandList_Close(bundle);
    ok(SUCCEEDED(hr), "Failed to close bundle, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_ExecuteBundle(command_list, bundle);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < rb.height; ++y)
    {
        for (x = 0; x < rb.width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           /* This works on AMD. */
           ok(v == 0xffffffff || v == 0xff00ff00, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    reset_command_list(command_list, context.allocator);
    reset_command_list(bundle, bundle_allocator);

    /* A bundle does not inherit the current primitive topology. */
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);

    ID3D12GraphicsCommandList_SetPipelineState(bundle, context.pipeline_state);
    ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
    hr = ID3D12GraphicsCommandList_Close(bundle);
    ok(SUCCEEDED(hr), "Failed to close bundle, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_ExecuteBundle(command_list, bundle);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < rb.height; ++y)
    {
        for (x = 0; x < rb.width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           /* This works on AMD, even though the debug layer says that the primitive topology is undefined. */
           ok(v == 0xffffffff || v == 0xff00ff00, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    reset_command_list(command_list, context.allocator);
    reset_command_list(bundle, bundle_allocator);

    /* A bundle inherit all other states. */
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);

    ID3D12GraphicsCommandList_SetPipelineState(bundle, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(bundle, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
    hr = ID3D12GraphicsCommandList_Close(bundle);
    ok(SUCCEEDED(hr), "Failed to close bundle, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_ExecuteBundle(command_list, bundle);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target, 0, queue, command_list, 0xff00ff00, 0);

    reset_command_list(command_list, context.allocator);
    reset_command_list(bundle, bundle_allocator);

    /* All state that is set in a bundle affects a command list. */
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(bundle, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(bundle, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(bundle, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    hr = ID3D12GraphicsCommandList_Close(bundle);
    ok(SUCCEEDED(hr), "Failed to close bundle, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_ExecuteBundle(command_list, bundle);

    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target, 0, queue, command_list, 0xff00ff00, 0);

    ID3D12CommandAllocator_Release(bundle_allocator);
    ID3D12GraphicsCommandList_Release(bundle);
    destroy_test_context(&context);
}

static void test_shader_instructions(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    const D3D12_SHADER_BYTECODE *current_ps;
    ID3D12GraphicsCommandList *command_list;
    struct test_context_desc desc;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    unsigned int i, x, y;
    ID3D12Resource *cb;

    static const DWORD ps_div_code[] =
    {
#if 0
        float4 src0;
        float4 src1;

        void main(out float4 dst : SV_Target)
        {
            dst.x = src0.x / src1.x;
            dst.yzw = (float3)0;
        }
#endif
        0x43425844, 0x19578813, 0xb1e4de1e, 0x3adee1dc, 0x478cd5d3, 0x00000001, 0x000000e8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000070, 0x00000050, 0x0000001c,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x0900000e, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020800a, 0x00000000,
        0x00000001, 0x08000036, 0x001020e2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_div = {ps_div_code, sizeof(ps_div_code)};
    static const DWORD ps_dot2_code[] =
    {
#if 0
        float4 src0;
        float4 src1;

        void main(out float4 dst : SV_Target)
        {
            dst.x = dot(src0.xy, src1.xy);
            dst.yzw = (float3)0;
        }
#endif
        0x43425844, 0x3621a1c7, 0x79d3be21, 0x9f14138c, 0x9f5506f2, 0x00000001, 0x000000e8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000070, 0x00000050, 0x0000001c,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x0900000f, 0x00102012, 0x00000000, 0x00208046, 0x00000000, 0x00000000, 0x00208046, 0x00000000,
        0x00000001, 0x08000036, 0x001020e2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_dot2 = {ps_dot2_code, sizeof(ps_dot2_code)};
    static const DWORD ps_eq_code[] =
    {
#if 0
        float4 src0;
        float4 src1;

        void main(out float4 dst : SV_Target)
        {
            dst = (uint4)0;
            if (src0.x == src1.x)
                dst.x = asfloat(0xffffffff);
        }
#endif
        0x43425844, 0x7bce1728, 0xa7d5d0f0, 0xaef5bc00, 0x7bb6b161, 0x00000001, 0x000000e8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000070, 0x00000050, 0x0000001c,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x09000018, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020800a, 0x00000000,
        0x00000001, 0x08000036, 0x001020e2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_eq = {ps_eq_code, sizeof(ps_eq_code)};
    static const DWORD ps_ne_code[] =
    {
#if 0
        float4 src0;
        float4 src1;

        void main(out float4 dst : SV_Target)
        {
            dst = (uint4)0;
            if (src0.x != src1.x)
                dst.x = asfloat(0xffffffff);
        }
#endif
        0x43425844, 0x5bbb7f90, 0x1a44971c, 0x4ee3d92e, 0x149ceecf, 0x00000001, 0x000000e8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000070, 0x00000050, 0x0000001c,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x09000039, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020800a, 0x00000000,
        0x00000001, 0x08000036, 0x001020e2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ne = {ps_ne_code, sizeof(ps_ne_code)};
    static const DWORD ps_if_code[] =
    {
        /* compiled with /Gfp option */
#if 0
        float4 src0;

        void main(out float4 dst : SV_Target)
        {
            if (src0.x)
                dst = float4(0, 1, 0, 1);
            else
                dst = float4(1, 0, 0, 1);
        }
#endif
        0x43425844, 0xfe5b6a47, 0x123f8934, 0xfa5910fe, 0x497aad93, 0x00000001, 0x0000012c, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000b4, 0x00000050, 0x0000002d,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x0b000039, 0x00100012, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000,
        0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000,
        0x01000012, 0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x00000000, 0x00000000,
        0x3f800000, 0x01000015, 0x0100003e
    };
    static const D3D12_SHADER_BYTECODE ps_if = {ps_if_code, sizeof(ps_if_code)};
    static const DWORD ps_if_return_code[] =
    {
#if 0
        float4 src0;

        void main(out float4 dst : SV_Target)
        {
            dst = (float4)0;
            if (src0.x < 4)
                return;
            dst.x = 1;
            if (src0.y < 4)
                return;
            dst.y = 1;
            if (src0.z >= 4)
                return;
            dst.z = 1;
            if (src0.w <= src0.x)
                return;
            dst.w = 1;
        }
#endif
        0x43425844, 0xa2797349, 0xd0a60aee, 0x7ae89f23, 0xf9681bfe, 0x00000001, 0x00000220, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000001a8, 0x00000050, 0x0000006a,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x08000031, 0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000,
        0x00004001, 0x40800000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e, 0x01000015, 0x08000031,
        0x00100012, 0x00000000, 0x0020801a, 0x00000000, 0x00000000, 0x00004001, 0x40800000, 0x0304001f,
        0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x00000000,
        0x00000000, 0x00000000, 0x0100003e, 0x01000015, 0x0800001d, 0x00100012, 0x00000000, 0x0020802a,
        0x00000000, 0x00000000, 0x00004001, 0x40800000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x3f800000, 0x00000000, 0x00000000, 0x0100003e,
        0x01000015, 0x0900001d, 0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020803a,
        0x00000000, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x3f800000, 0x3f800000, 0x3f800000, 0x00000000, 0x0100003e, 0x01000015, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_if_return = {ps_if_return_code, sizeof(ps_if_return_code)};
    static const DWORD ps_nested_if_code[] =
    {
        /* compiled with /Gfp option */
#if 0
        float4 src0;

        void main(out float4 dst : SV_Target)
        {
            if (src0.x)
            {
                if (src0.y)
                    dst = float4(0, 1, 0, 1);
                else
                    dst = float4(0, 0, 1, 1);
            }
            else
            {
                if (src0.z)
                    dst = float4(1, 0, 0, 1);
                else
                    dst = float4(0, 0, 0, 1);
            }
        }
#endif
        0x43425844, 0x35e50e88, 0x68c45bdd, 0x0dc60de1, 0x4bc29735, 0x00000001, 0x000001ec, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000174, 0x00000050, 0x0000005d,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x0b000039, 0x00100012, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000,
        0x0b000039, 0x00100012, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x0020801a, 0x00000000, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2,
        0x00000000, 0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000, 0x01000012, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x3f800000, 0x3f800000, 0x01000015,
        0x01000012, 0x0b000039, 0x00100012, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0020802a, 0x00000000, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x00000000, 0x00000000, 0x3f800000, 0x01000012,
        0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x3f800000,
        0x01000015, 0x01000015, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_nested_if = {ps_nested_if_code, sizeof(ps_nested_if_code)};
    static const DWORD ps_loop_break_code[] =
    {
#if 0
        float4 src0;

        void main(out float4 dst : SV_Target)
        {
            float tmp = 0.0f;
            for (int i = 0; i < src0.x; ++i)
            {
                if (i == src0.y)
                {
                    tmp = 1.0f;
                    break;
                }
                tmp += 1.0f;
            }

            dst = float4(tmp, 0, 0, 0);
        }
#endif
        0x43425844, 0xbd9dabbd, 0xe56cab2a, 0xfd37cd76, 0x5dd181c4, 0x00000001, 0x000001c8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000150, 0x00000050, 0x00000054,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x08000036, 0x00100032, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x01000030, 0x0500002b, 0x00100042, 0x00000000, 0x0010001a, 0x00000000,
        0x0800001d, 0x00100082, 0x00000000, 0x0010002a, 0x00000000, 0x0020800a, 0x00000000, 0x00000000,
        0x03040003, 0x0010003a, 0x00000000, 0x08000018, 0x00100042, 0x00000000, 0x0010002a, 0x00000000,
        0x0020801a, 0x00000000, 0x00000000, 0x0304001f, 0x0010002a, 0x00000000, 0x05000036, 0x00100012,
        0x00000000, 0x00004001, 0x3f800000, 0x01000002, 0x01000015, 0x07000000, 0x00100012, 0x00000000,
        0x0010000a, 0x00000000, 0x00004001, 0x3f800000, 0x0700001e, 0x00100022, 0x00000000, 0x0010001a,
        0x00000000, 0x00004001, 0x00000001, 0x01000016, 0x05000036, 0x00102012, 0x00000000, 0x0010000a,
        0x00000000, 0x08000036, 0x001020e2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_loop_break = {ps_loop_break_code, sizeof(ps_loop_break_code)};
    static const DWORD ps_loop_ret_code[] =
    {
#if 0
        float4 src0;

        void main(out float4 dst : SV_Target)
        {
            float tmp = 0.0f;
            for (int i = 0; i < src0.x; ++i)
            {
                if (i == src0.y)
                {
                    dst = 1;
                    return;
                }
                tmp += 1.0f;
            }

            dst = float4(tmp, 0, 0, 0);
        }
#endif
        0x43425844, 0xb327003a, 0x5812a5f6, 0xb5a78d54, 0xa72a8db8, 0x00000001, 0x000001d4, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000015c, 0x00000050, 0x00000057,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x08000036, 0x00100032, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x01000030, 0x0500002b, 0x00100042, 0x00000000, 0x0010001a, 0x00000000,
        0x0800001d, 0x00100082, 0x00000000, 0x0010002a, 0x00000000, 0x0020800a, 0x00000000, 0x00000000,
        0x03040003, 0x0010003a, 0x00000000, 0x08000018, 0x00100042, 0x00000000, 0x0010002a, 0x00000000,
        0x0020801a, 0x00000000, 0x00000000, 0x0304001f, 0x0010002a, 0x00000000, 0x08000036, 0x001020f2,
        0x00000000, 0x00004002, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x0100003e, 0x01000015,
        0x07000000, 0x00100012, 0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x3f800000, 0x0700001e,
        0x00100022, 0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x00000001, 0x01000016, 0x05000036,
        0x00102012, 0x00000000, 0x0010000a, 0x00000000, 0x08000036, 0x001020e2, 0x00000000, 0x00004002,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_loop_ret = {ps_loop_ret_code, sizeof(ps_loop_ret_code)};
    static const DWORD ps_breakc_nz_code[] =
    {
#if 0
        float4 main() : SV_TARGET
        {
            uint counter = 0;

            for (uint i = 0; i < 255; ++i)
                ++counter;

            if (counter == 255)
                return float4(0.0f, 1.0f, 0.0f, 1.0f);
            else
                return float4(1.0f, 0.0f, 0.0f, 1.0f);
        }
#endif
        0x43425844, 0x065ac80a, 0x24369e7e, 0x218d5dc1, 0x3532868c, 0x00000001, 0x00000188, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x45475241, 0xabab0054, 0x52444853, 0x00000110, 0x00000040, 0x00000044,
        0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x08000036, 0x00100032, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000030, 0x07000050, 0x00100042,
        0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x000000ff, 0x03040003, 0x0010002a, 0x00000000,
        0x0a00001e, 0x00100032, 0x00000000, 0x00100046, 0x00000000, 0x00004002, 0x00000001, 0x00000001,
        0x00000000, 0x00000000, 0x01000016, 0x07000020, 0x00100012, 0x00000000, 0x0010000a, 0x00000000,
        0x00004001, 0x000000ff, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000, 0x0100003e, 0x01000012, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x00000000, 0x00000000, 0x3f800000, 0x0100003e,
        0x01000015, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_breakc_nz = {ps_breakc_nz_code, sizeof(ps_breakc_nz_code)};
    static const DWORD ps_breakc_z_code[] =
    {
#if 0
        float4 main() : SV_TARGET
        {
            uint counter = 0;

            for (int i = 0, j = 254; i < 255 && j >= 0; ++i, --j)
                ++counter;

            if (counter == 255)
                return float4(0.0f, 1.0f, 0.0f, 1.0f);
            else
                return float4(1.0f, 0.0f, 0.0f, 1.0f);
        }
#endif
        0x43425844, 0x687406ef, 0x7bdeb7d1, 0xb3282292, 0x934a9101, 0x00000001, 0x000001c0, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x45475241, 0xabab0054, 0x52444853, 0x00000148, 0x00000040, 0x00000052,
        0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000002, 0x08000036, 0x00100072, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x000000fe, 0x00000000, 0x01000030, 0x07000022, 0x00100082,
        0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x000000ff, 0x07000021, 0x00100012, 0x00000001,
        0x0010002a, 0x00000000, 0x00004001, 0x00000000, 0x07000001, 0x00100082, 0x00000000, 0x0010003a,
        0x00000000, 0x0010000a, 0x00000001, 0x03000003, 0x0010003a, 0x00000000, 0x0a00001e, 0x00100072,
        0x00000000, 0x00100246, 0x00000000, 0x00004002, 0x00000001, 0x00000001, 0xffffffff, 0x00000000,
        0x01000016, 0x07000020, 0x00100012, 0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x000000ff,
        0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x00000000,
        0x3f800000, 0x00000000, 0x3f800000, 0x0100003e, 0x01000012, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x3f800000, 0x00000000, 0x00000000, 0x3f800000, 0x0100003e, 0x01000015, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_breakc_z = {ps_breakc_z_code, sizeof(ps_breakc_z_code)};
    static const DWORD ps_src_modifiers_code[] =
    {
#if 0
        float4 src0;

        void main(out float4 dst : SV_Target)
        {
            dst.x = -src0.x;
            dst.y = abs(src0.y);
            dst.zw = -abs(src0.zw);
        }
#endif
        0x43425844, 0xa5f66fa8, 0xd430e547, 0x1cd28240, 0xaf5bc0f4, 0x00000001, 0x000000f8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000080, 0x00000050, 0x00000020,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x07000036, 0x00102012, 0x00000000, 0x8020800a, 0x00000041, 0x00000000, 0x00000000, 0x07000036,
        0x00102022, 0x00000000, 0x8020801a, 0x00000081, 0x00000000, 0x00000000, 0x07000036, 0x001020c2,
        0x00000000, 0x80208ea6, 0x000000c1, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_src_modifiers = {ps_src_modifiers_code, sizeof(ps_src_modifiers_code)};
    static const DWORD ps_sat_code[] =
    {
#if 0
        float4 src;

        void main(out float4 dst : SV_Target)
        {
            dst = clamp(src, 0, 1);
        }
#endif
        0x43425844, 0x50af2f8b, 0xaadad7cd, 0x77815f01, 0x612ec066, 0x00000001, 0x000000bc, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000044, 0x00000050, 0x00000011,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x06002036, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_sat = {ps_sat_code, sizeof(ps_sat_code)};
    static const DWORD ps_min_max_code[] =
    {
#if 0
        float4 src0;
        float4 src1;

        void main(out float4 dst : SV_Target)
        {
            dst = (float4)0;
            dst.x = min(src0.x, src1.x);
            dst.y = max(src0.x, src1.x);
        }
#endif
        0x43425844, 0xb570ee39, 0xcf84fe48, 0x7fa59ede, 0x6151def2, 0x00000001, 0x0000010c, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000094, 0x00000050, 0x00000025,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x09000033, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020800a, 0x00000000,
        0x00000001, 0x09000034, 0x00102022, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020800a,
        0x00000000, 0x00000001, 0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_min_max = {ps_min_max_code, sizeof(ps_min_max_code)};
    static const DWORD ps_ftou_code[] =
    {
#if 0
        float src;

        void main(out float4 dst : SV_Target)
        {
            dst = asfloat(uint4(src, -src, 0, 0));
        }
#endif
        0x43425844, 0x7a61c2fa, 0x4f20de14, 0x3492a5ae, 0x0a1fdc98, 0x00000001, 0x000000f8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000080, 0x00000050, 0x00000020,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0600001c, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0700001c, 0x00102022,
        0x00000000, 0x8020800a, 0x00000041, 0x00000000, 0x00000000, 0x08000036, 0x001020c2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ftou = {ps_ftou_code, sizeof(ps_ftou_code)};
    static const DWORD ps_ftoi_code[] =
    {
#if 0
        float src;

        void main(out float4 dst : SV_Target)
        {
            dst = asfloat(int4(src, -src, 0, 0));
        }
#endif
        0x43425844, 0x2737f059, 0x5a2faecc, 0x7eab1956, 0xf96357b5, 0x00000001, 0x000000f8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000080, 0x00000050, 0x00000020,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0600001b, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0700001b, 0x00102022,
        0x00000000, 0x8020800a, 0x00000041, 0x00000000, 0x00000000, 0x08000036, 0x001020c2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ftoi = {ps_ftoi_code, sizeof(ps_ftoi_code)};
    static const DWORD ps_round_code[] =
    {
#if 0
        float src0;

        void main(out float4 dst : SV_Target)
        {
            dst.x = floor(src0);
            dst.y = ceil(src0);
            dst.z = trunc(src0);
            dst.w = 0;
        }
#endif
        0x43425844, 0x44e2c554, 0x216a8c83, 0x87e90dd8, 0x3fde3e57, 0x00000001, 0x00000100, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000088, 0x00000050, 0x00000022,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x06000041, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x06000042, 0x00102022,
        0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x06000043, 0x00102042, 0x00000000, 0x0020800a,
        0x00000000, 0x00000000, 0x05000036, 0x00102082, 0x00000000, 0x00004001, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_round = {ps_round_code, sizeof(ps_round_code)};
    static const DWORD ps_frc_code[] =
    {
#if 0
        float src;

        void main(out float4 dst : SV_Target)
        {
            dst = 0;
            dst.x = frac(src);
            dst.y = frac(-src);
        }
#endif
        0x43425844, 0xd52bc741, 0xda411d9a, 0x199054a2, 0x7461462d, 0x00000001, 0x000000f8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000080, 0x00000050, 0x00000020,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0600001a, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0700001a, 0x00102022,
        0x00000000, 0x8020800a, 0x00000041, 0x00000000, 0x00000000, 0x08000036, 0x001020c2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_frc = {ps_frc_code, sizeof(ps_frc_code)};
    static const DWORD ps_exp_code[] =
    {
#if 0
        float src;

        void main(out float4 dst : SV_Target)
        {
            dst = 0;
            dst.x = exp2(src);
        }
#endif
        0x43425844, 0xa742b300, 0x10b64393, 0x7827fc4a, 0x328b8db5, 0x00000001, 0x000000dc, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000064, 0x00000050, 0x00000019,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x06000019, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x08000036, 0x001020e2,
        0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_exp = {ps_exp_code, sizeof(ps_exp_code)};
    static const DWORD ps_log_code[] =
    {
#if 0
        float src;

        void main(out float4 dst : SV_Target)
        {
            dst = 0;
            dst.x = log2(src);
        }
#endif
        0x43425844, 0x2f1cc195, 0x6cc7d061, 0xe63df3b1, 0x9c68b968, 0x00000001, 0x000000dc, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000064, 0x00000050, 0x00000019,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0600002f, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x08000036, 0x001020e2,
        0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_log = {ps_log_code, sizeof(ps_log_code)};
    static const DWORD ps_rcp_code[] =
    {
#if 0
        float4 src;

        void main(out float4 dst : SV_Target)
        {
            dst = 0;
            dst.x = rcp(src.x);
        }
#endif
        0x43425844, 0x3b0ab43e, 0xff4dcb50, 0x22531bf6, 0xe44bbc8c, 0x00000001, 0x000000dc, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000064, 0x00000050, 0x00000019,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x06000081, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x08000036, 0x001020e2,
        0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_rcp = {ps_rcp_code, sizeof(ps_rcp_code)};
    static const DWORD ps_bfi_code[] =
    {
#if 0
        uint bits, offset, insert, base;

        uint4 main() : SV_Target
        {
            uint mask = ((1 << bits) - 1) << offset;
            return ((insert << offset) & mask) | (base & ~mask);
        }
#endif
        0x43425844, 0xbe9af688, 0xf5caec6f, 0x63ed2522, 0x5f91f209, 0x00000001, 0x000000e0, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000068, 0x00000050, 0x0000001a,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0f00008c, 0x001020f2, 0x00000000, 0x00208006, 0x00000000, 0x00000000, 0x00208556, 0x00000000,
        0x00000000, 0x00208aa6, 0x00000000, 0x00000000, 0x00208ff6, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_bfi = {ps_bfi_code, sizeof(ps_bfi_code)};
    static const DWORD ps_ibfe_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer cb0[1], immediateIndexed
        dcl_output o0.xyzw
        ibfe o0.xyzw, cb0[0].xxxx, cb0[0].yyyy, cb0[0].zzzz
        ret
#endif
        0x43425844, 0x4b2225f7, 0xd0860f66, 0xe38775bb, 0x6d23d1d2, 0x00000001, 0x000000d4, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000005c, 0x00000050, 0x00000017,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0c00008b, 0x001020f2, 0x00000000, 0x00208006, 0x00000000, 0x00000000, 0x00208556, 0x00000000,
        0x00000000, 0x00208aa6, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ibfe = {ps_ibfe_code, sizeof(ps_ibfe_code)};
    static const DWORD ps_ubfe_code[] =
    {
#if 0
        uint u;

        uint4 main() : SV_Target
        {
            return uint4((u & 0xf0) >> 4, (u & 0x7fffff00) >> 8, (u & 0xfe) >> 1, (u & 0x7fffffff) >> 1);
        }
#endif
        0x43425844, 0xc4ac0509, 0xaea83154, 0xf1fb3b80, 0x4c22e3cc, 0x00000001, 0x000000e4, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000006c, 0x00000050, 0x0000001b,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x1000008a, 0x001020f2, 0x00000000, 0x00004002, 0x00000004, 0x00000017, 0x00000007, 0x0000001e,
        0x00004002, 0x00000004, 0x00000008, 0x00000001, 0x00000001, 0x00208006, 0x00000000, 0x00000000,
        0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ubfe = {ps_ubfe_code, sizeof(ps_ubfe_code)};
    static const DWORD ps_bfrev_code[] =
    {
#if 0
        uint bits;

        uint4 main() : SV_Target
        {
            return uint4(reversebits(bits), reversebits(reversebits(bits)),
                    reversebits(bits & 0xFFFF), reversebits(bits >> 16));
        }
#endif
        0x43425844, 0x73daef82, 0xe52befa3, 0x8504d5f0, 0xebdb321d, 0x00000001, 0x00000154, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000dc, 0x00000050, 0x00000037,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x08000001, 0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000,
        0x00004001, 0x0000ffff, 0x0500008d, 0x00102042, 0x00000000, 0x0010000a, 0x00000000, 0x08000055,
        0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x00004001, 0x00000010, 0x0500008d,
        0x00102082, 0x00000000, 0x0010000a, 0x00000000, 0x0600008d, 0x00100012, 0x00000000, 0x0020800a,
        0x00000000, 0x00000000, 0x0500008d, 0x00102022, 0x00000000, 0x0010000a, 0x00000000, 0x05000036,
        0x00102012, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_bfrev = {ps_bfrev_code, sizeof(ps_bfrev_code)};
    static const DWORD ps_bits_code[] =
    {
#if 0
        uint u;
        int i;

        uint4 main() : SV_Target
        {
            return uint4(countbits(u), firstbitlow(u), firstbithigh(u), firstbithigh(i));
        }
#endif
        0x43425844, 0x23fee911, 0x145287d1, 0xea904419, 0x8aa59a6a, 0x00000001, 0x000001b4, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000013c, 0x00000050, 0x0000004f,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x06000089, 0x00100012, 0x00000000, 0x0020801a, 0x00000000, 0x00000000,
        0x07000020, 0x00100022, 0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0xffffffff, 0x0800001e,
        0x00100012, 0x00000000, 0x00004001, 0x0000001f, 0x8010000a, 0x00000041, 0x00000000, 0x09000037,
        0x00102082, 0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0xffffffff, 0x0010000a, 0x00000000,
        0x06000087, 0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0800001e, 0x00100012,
        0x00000000, 0x00004001, 0x0000001f, 0x8010000a, 0x00000041, 0x00000000, 0x0a000037, 0x00102042,
        0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0xffffffff,
        0x06000086, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x06000088, 0x00102022,
        0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_bits = {ps_bits_code, sizeof(ps_bits_code)};
    static const DWORD ps_ishr_code[] =
    {
#if 0
        int4 src0;
        int4 src1;

        void main(out uint4 dst : SV_Target)
        {
            dst = src0 >> src1;
        }
#endif
        0x43425844, 0x4551d737, 0xd3dcf723, 0xdf387a99, 0xb6d6b00b, 0x00000001, 0x000000c8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000050, 0x00000050, 0x00000014,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x0900002a, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00208e46, 0x00000000,
        0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ishr = {ps_ishr_code, sizeof(ps_ishr_code)};
    static const DWORD ps_ushr_code[] =
    {
#if 0
        uint4 src0;
        uint4 src1;

        void main(out uint4 dst : SV_Target)
        {
            dst = src0 >> src1;
        }
#endif
        0x43425844, 0x00f49f17, 0xe7933d92, 0xf527d4e6, 0x1fe1c216, 0x00000001, 0x000000c8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000050, 0x00000050, 0x00000014,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x09000055, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00208e46, 0x00000000,
        0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ushr = {ps_ushr_code, sizeof(ps_ushr_code)};
    static const DWORD ps_ishl_code[] =
    {
#if 0
        uint4 src0;
        uint4 src1;

        void main(out uint4 dst : SV_Target)
        {
            dst = src0 << src1;
        }
#endif
        0x43425844, 0xc88f5e4d, 0x64e1e5c6, 0x70e7173e, 0x960d6691, 0x00000001, 0x000000c8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000050, 0x00000050, 0x00000014,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x09000029, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00208e46, 0x00000000,
        0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ishl = {ps_ishl_code, sizeof(ps_ishl_code)};
    static const DWORD ps_not_code[] =
    {
#if 0
        uint2 bits;

        uint4 main() : SV_Target
        {
            return uint4(~bits.x, ~(bits.x ^ ~0u), ~bits.y, ~(bits.y ^ ~0u));
        }
#endif
        0x43425844, 0xaed0fd26, 0xf719a878, 0xc832efd6, 0xba03c264, 0x00000001, 0x00000100, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x00000088, 0x00000040, 0x00000022,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068,
        0x00000001, 0x0b000057, 0x00100032, 0x00000000, 0x00208046, 0x00000000, 0x00000000, 0x00004002,
        0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x0500003b, 0x001020a2, 0x00000000, 0x00100406,
        0x00000000, 0x0600003b, 0x00102052, 0x00000000, 0x00208106, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_not = {ps_not_code, sizeof(ps_not_code)};
    static const DWORD ps_icmp_code[] =
    {
#if 0
        int2 src;

        void main(out uint4 dst : SV_Target)
        {
            dst.x = src.x == src.y ? ~0u : 0;
            dst.y = src.x >= src.y ? ~0u : 0;
            dst.z = src.x < src.y  ? ~0u : 0;
            dst.w = src.x != src.y ? ~0u : 0;
        }
#endif
        0x43425844, 0xa39748f0, 0x39d5c9e4, 0xdf073d48, 0x7946c5c4, 0x00000001, 0x00000134, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000bc, 0x00000050, 0x0000002f,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x09000020, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020801a, 0x00000000,
        0x00000000, 0x09000021, 0x00102022, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020801a,
        0x00000000, 0x00000000, 0x09000022, 0x00102042, 0x00000000, 0x0020800a, 0x00000000, 0x00000000,
        0x0020801a, 0x00000000, 0x00000000, 0x09000027, 0x00102082, 0x00000000, 0x0020800a, 0x00000000,
        0x00000000, 0x0020801a, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_icmp = {ps_icmp_code, sizeof(ps_icmp_code)};
    static const DWORD ps_ucmp_code[] =
    {
#if 0
        uint2 src;

        void main(out uint4 dst : SV_Target)
        {
            dst = 0;
            dst.x = src.x >= src.y ? ~0u : 0;
            dst.y = src.x < src.y  ? ~0u : 0;
        }
#endif
        0x43425844, 0xe083954f, 0xb55bf642, 0xeb2fa36c, 0x60ee1782, 0x00000001, 0x0000010c, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000094, 0x00000050, 0x00000025,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x09000050, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020801a, 0x00000000,
        0x00000000, 0x0900004f, 0x00102022, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020801a,
        0x00000000, 0x00000000, 0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ucmp = {ps_ucmp_code, sizeof(ps_ucmp_code)};
    static const DWORD ps_umin_umax_code[] =
    {
#if 0
        uint2 src;

        void main(out uint4 dst : SV_Target)
        {
            dst.x = min(src.x, src.y);
            dst.y = max(src.x, src.y);
            dst.zw = 0;
        }
#endif
        0x43425844, 0xe705f812, 0xa515c8df, 0xb82066d9, 0xb05c8ad3, 0x00000001, 0x0000010c, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000094, 0x00000050, 0x00000025,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x09000054, 0x00102012, 0x00000000, 0x0020801a, 0x00000000, 0x00000000, 0x0020800a, 0x00000000,
        0x00000000, 0x09000053, 0x00102022, 0x00000000, 0x0020801a, 0x00000000, 0x00000000, 0x0020800a,
        0x00000000, 0x00000000, 0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_umin_umax = {ps_umin_umax_code, sizeof(ps_umin_umax_code)};
    static const DWORD ps_f16tof32_code[] =
    {
#if 0
        uint4 hf;

        uint4 main() : SV_Target
        {
            return f16tof32(hf);
        }
#endif
        0x43425844, 0xc1816e6e, 0x27562d96, 0x56980fa2, 0x421e6640, 0x00000001, 0x000000d8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000060, 0x00000050, 0x00000018,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x06000083, 0x001000f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000,
        0x0500001c, 0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_f16tof32 = {ps_f16tof32_code, sizeof(ps_f16tof32_code)};
    static const DWORD ps_f32tof16_code[] =
    {
#if 0
        float4 f;

        uint4 main() : SV_Target
        {
            return f32tof16(f);
        }
#endif
        0x43425844, 0x523a765c, 0x1a5be3a9, 0xaed69c80, 0xd26fe296, 0x00000001, 0x000000bc, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000044, 0x00000050, 0x00000011,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x06000082, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_f32tof16 = {ps_f32tof16_code, sizeof(ps_f32tof16_code)};
    static const DWORD ps_imad_code[] =
    {
#if 0
        int4 src0;
        int4 src1;
        int4 src2;

        void main(out uint4 dst : SV_Target)
        {
            dst.xy = src0.xy * src1.xy + src2.xy;
            dst.zw = src0.zw * src1.zw - src2.zw;
        }
#endif
        0x43425844, 0xb6a7735a, 0xc891e560, 0x6df8f267, 0x2753395c, 0x00000001, 0x00000108, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000090, 0x00000050, 0x00000024,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000003, 0x03000065, 0x001020f2, 0x00000000,
        0x0c000023, 0x00102032, 0x00000000, 0x00208046, 0x00000000, 0x00000000, 0x00208046, 0x00000000,
        0x00000001, 0x00208046, 0x00000000, 0x00000002, 0x0d000023, 0x001020c2, 0x00000000, 0x00208ea6,
        0x00000000, 0x00000000, 0x00208ea6, 0x00000000, 0x00000001, 0x80208ea6, 0x00000041, 0x00000000,
        0x00000002, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_imad = {ps_imad_code, sizeof(ps_imad_code)};
    static const DWORD ps_imul_code[] =
    {
#if 0
        uint4 src0;
        uint4 src1;

        void main(out uint4 dst : SV_Target)
        {
            dst = 0;
            dst.x = src0.x * src1.x;
        }
#endif
        0x43425844, 0x55ebfe14, 0xc9834c14, 0x5f89388a, 0x523be7e0, 0x00000001, 0x000000ec, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000074, 0x00000050, 0x0000001d,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x0a000026, 0x0000d000, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020800a,
        0x00000000, 0x00000001, 0x08000036, 0x001020e2, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_imul = {ps_imul_code, sizeof(ps_imul_code)};
    static const DWORD ps_udiv_code[] =
    {
#if 0
        uint4 src0;
        uint4 src1;

        void main(out uint4 dst : SV_Target)
        {
            dst = 0;
            dst.x = src0.x / src1.x;
            dst.y = src0.x % src1.x;
        }
#endif
        0x43425844, 0x007d5f29, 0x042f2e56, 0x212eddf2, 0xc98cca76, 0x00000001, 0x00000120, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000a8, 0x00000050, 0x0000002a,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000002, 0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x0b00004e, 0x00100012, 0x00000000, 0x00100012, 0x00000001, 0x0020800a,
        0x00000000, 0x00000000, 0x0020800a, 0x00000000, 0x00000001, 0x05000036, 0x00102012, 0x00000000,
        0x0010000a, 0x00000000, 0x05000036, 0x00102022, 0x00000000, 0x0010000a, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_udiv = {ps_udiv_code, sizeof(ps_udiv_code)};
    static const DWORD ps_nested_switch_code[] =
    {
#if 0
        uint4 src0;
        uint4 src1;

        uint4 main() : SV_Target
        {
            uint4 dst = 0;

            switch (src0.x)
            {
                case ~0u:
                    dst.x = 1;
                    break;
                case 0:
                case 1:
                case 2:
                    if (src1.x)
                        break;
                    dst.x = 2;
                    break;
                case 3:
                    break;
                case 4:
                    if (src1.x)
                    {
                        switch (src0.y)
                        {
                            case 0:
                            case 1:
                            case 2:
                            case 3:
                                if (src0.z)
                                    dst += src0.z * (uint4)2;
                                else if (src0.w)
                                    return (uint4)255;
                                else
                                    dst.zw = 1;
                                break;
                            default:
                                dst = 1;
                                break;
                        }
                        break;
                    }
                    else
                    {
                        dst = 128;
                        break;
                    }
            }

            return dst;
        }
#endif
        0x43425844, 0x46d465cb, 0x5d7ed52f, 0x3573b153, 0x1691c479, 0x00000001, 0x00000334, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000002bc, 0x00000050, 0x000000af,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x0400004c, 0x0020800a, 0x00000000, 0x00000000, 0x03000006, 0x00004001,
        0xffffffff, 0x08000036, 0x001000f2, 0x00000000, 0x00004002, 0x00000001, 0x00000000, 0x00000000,
        0x00000000, 0x01000002, 0x03000006, 0x00004001, 0x00000000, 0x03000006, 0x00004001, 0x00000001,
        0x03000006, 0x00004001, 0x00000002, 0x0404001f, 0x0020800a, 0x00000000, 0x00000001, 0x08000036,
        0x001000f2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000002,
        0x01000015, 0x08000036, 0x001000f2, 0x00000000, 0x00004002, 0x00000002, 0x00000000, 0x00000000,
        0x00000000, 0x01000002, 0x03000006, 0x00004001, 0x00000003, 0x08000036, 0x001000f2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000002, 0x03000006, 0x00004001,
        0x00000004, 0x0404001f, 0x0020800a, 0x00000000, 0x00000001, 0x0400004c, 0x0020801a, 0x00000000,
        0x00000000, 0x03000006, 0x00004001, 0x00000000, 0x03000006, 0x00004001, 0x00000001, 0x03000006,
        0x00004001, 0x00000002, 0x03000006, 0x00004001, 0x00000003, 0x0404001f, 0x0020802a, 0x00000000,
        0x00000000, 0x0b000029, 0x001000f2, 0x00000000, 0x00208aa6, 0x00000000, 0x00000000, 0x00004002,
        0x00000001, 0x00000001, 0x00000001, 0x00000001, 0x01000012, 0x0404001f, 0x0020803a, 0x00000000,
        0x00000000, 0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x000000ff, 0x000000ff, 0x000000ff,
        0x000000ff, 0x0100003e, 0x01000015, 0x08000036, 0x001000f2, 0x00000000, 0x00004002, 0x00000000,
        0x00000000, 0x00000001, 0x00000001, 0x01000015, 0x01000002, 0x0100000a, 0x08000036, 0x001000f2,
        0x00000000, 0x00004002, 0x00000001, 0x00000001, 0x00000001, 0x00000001, 0x01000002, 0x01000017,
        0x01000002, 0x01000012, 0x08000036, 0x001000f2, 0x00000000, 0x00004002, 0x00000080, 0x00000080,
        0x00000080, 0x00000080, 0x01000002, 0x01000015, 0x0100000a, 0x08000036, 0x001000f2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000002, 0x01000017, 0x05000036,
        0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_nested_switch = {ps_nested_switch_code, sizeof(ps_nested_switch_code)};
    static const DWORD ps_switch_no_default_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer CB0[1], immediateIndexed
        dcl_output o0.xyzw
        switch cb0[0].x
            case l(0)
            mov o0.xyzw, l(1,1,1,1)
            ret
            case l(3)
            mov o0.xyzw, l(2,2,2,2)
            ret
        endswitch
        nop
        nop
        mov o0.xyzw, l(3,3,3,3)
        ret
#endif
        0x43425844, 0x97459226, 0x57ed7614, 0xcda58342, 0xbdf6a9dd, 0x00000001, 0x00000140, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000c8, 0x00000050, 0x00000032,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0400004c, 0x0020800a, 0x00000000, 0x00000000, 0x03000006, 0x00004001, 0x00000000, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x00000001, 0x00000001, 0x00000001, 0x00000001, 0x0100003e,
        0x03000006, 0x00004001, 0x00000003, 0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x00000002,
        0x00000002, 0x00000002, 0x00000002, 0x0100003e, 0x01000017, 0x0100003a, 0x0100003a, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x00000003, 0x00000003, 0x00000003, 0x00000003, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_switch_no_default = {ps_switch_no_default_code, sizeof(ps_switch_no_default_code)};
    static const DWORD ps_movc_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer cb0[3], immediateIndexed
        dcl_output o0.xyzw
        mov o0.xyzw, cb0[0].xyzw
        movc o0.xyzw, o0.xyzw, cb0[1].xyzw, cb0[2].xyzw
        ret
#endif
        0x43425844, 0x317dec99, 0x3a8928ca, 0x5db9a8ea, 0xb4806d11, 0x00000001, 0x000000e8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000070, 0x00000050, 0x0000001c,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000003, 0x03000065, 0x001020f2, 0x00000000,
        0x06000036, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x0b000037, 0x001020f2,
        0x00000000, 0x00102e46, 0x00000000, 0x00208e46, 0x00000000, 0x00000001, 0x00208e46, 0x00000000,
        0x00000002, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_movc = {ps_movc_code, sizeof(ps_movc_code)};
    static const DWORD ps_swapc0_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer cb0[3], immediateIndexed
        dcl_output o0.xyzw
        dcl_temps 2
        swapc r0.xyzw, r1.xyzw, cb0[0].xyzw, cb0[1].xyzw, cb0[2].xyzw
        mov o0.xyzw, r0.xyzw
        ret
#endif
        0x43425844, 0x9e089246, 0x9f8b5cbe, 0xbac66faf, 0xaef23488, 0x00000001, 0x000000f8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000080, 0x00000050, 0x00000020,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000003, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000002, 0x0e00008e, 0x001000f2, 0x00000000, 0x001000f2, 0x00000001, 0x00208e46,
        0x00000000, 0x00000000, 0x00208e46, 0x00000000, 0x00000001, 0x00208e46, 0x00000000, 0x00000002,
        0x05000036, 0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_swapc0 = {ps_swapc0_code, sizeof(ps_swapc0_code)};
    static const DWORD ps_swapc1_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer cb0[3], immediateIndexed
        dcl_output o0.xyzw
        dcl_temps 2
        swapc r0.xyzw, r1.xyzw, cb0[0].xyzw, cb0[1].xyzw, cb0[2].xyzw
        mov o0.xyzw, r1.xyzw
        ret
#endif
        0x43425844, 0xf2daed61, 0xede211f7, 0x7300dbea, 0x573ed49f, 0x00000001, 0x000000f8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000080, 0x00000050, 0x00000020,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000003, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000002, 0x0e00008e, 0x001000f2, 0x00000000, 0x001000f2, 0x00000001, 0x00208e46,
        0x00000000, 0x00000000, 0x00208e46, 0x00000000, 0x00000001, 0x00208e46, 0x00000000, 0x00000002,
        0x05000036, 0x001020f2, 0x00000000, 0x00100e46, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_swapc1 = {ps_swapc1_code, sizeof(ps_swapc1_code)};
    static const DWORD ps_swapc2_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer cb0[3], immediateIndexed
        dcl_output o0.xyzw
        dcl_temps 2
        mov r0.xyzw, cb0[1].xyzw
        mov r1.xyzw, cb0[2].xyzw
        swapc r0.xyzw, r1.xyzw, cb0[0].xyzw, r0.xyzw, r1.xyzw
        mov o0.xyzw, r0.xyzw
        ret
#endif
        0x43425844, 0x230fcb22, 0x70d99148, 0x65814d89, 0x97473498, 0x00000001, 0x00000120, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000a8, 0x00000050, 0x0000002a,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000003, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000002, 0x06000036, 0x001000f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000001,
        0x06000036, 0x001000f2, 0x00000001, 0x00208e46, 0x00000000, 0x00000002, 0x0c00008e, 0x001000f2,
        0x00000000, 0x001000f2, 0x00000001, 0x00208e46, 0x00000000, 0x00000000, 0x00100e46, 0x00000000,
        0x00100e46, 0x00000001, 0x05000036, 0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_swapc2 = {ps_swapc2_code, sizeof(ps_swapc2_code)};
    static const DWORD ps_swapc3_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer cb0[3], immediateIndexed
        dcl_output o0.xyzw
        dcl_temps 2
        mov r0.xyzw, cb0[1].xyzw
        mov r1.xyzw, cb0[2].xyzw
        swapc r0.xyzw, r1.xyzw, cb0[0].xyzw, r0.xyzw, r1.xyzw
        mov o0.xyzw, r1.xyzw
        ret
#endif
        0x43425844, 0xce595d62, 0x98305541, 0xb04e74c8, 0xfc010f3a, 0x00000001, 0x00000120, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000a8, 0x00000050, 0x0000002a,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000003, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000002, 0x06000036, 0x001000f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000001,
        0x06000036, 0x001000f2, 0x00000001, 0x00208e46, 0x00000000, 0x00000002, 0x0c00008e, 0x001000f2,
        0x00000000, 0x001000f2, 0x00000001, 0x00208e46, 0x00000000, 0x00000000, 0x00100e46, 0x00000000,
        0x00100e46, 0x00000001, 0x05000036, 0x001020f2, 0x00000000, 0x00100e46, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_swapc3 = {ps_swapc3_code, sizeof(ps_swapc3_code)};
    static const DWORD ps_swapc4_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer cb0[3], immediateIndexed
        dcl_output o0.xyzw
        dcl_temps 2
        mov r0.xyzw, cb0[0].xyzw
        swapc r0.xyzw, r1.xyzw, r0.xyzw, cb0[1].xyzw, cb0[2].xyzw
        mov o0.xyzw, r0.xyzw
        ret
#endif
        0x43425844, 0x72067c48, 0xb53572a0, 0x9dd9e0fd, 0x903e37e3, 0x00000001, 0x0000010c, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000094, 0x00000050, 0x00000025,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000003, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000002, 0x06000036, 0x001000f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000,
        0x0d00008e, 0x001000f2, 0x00000000, 0x001000f2, 0x00000001, 0x00100e46, 0x00000000, 0x00208e46,
        0x00000000, 0x00000001, 0x00208e46, 0x00000000, 0x00000002, 0x05000036, 0x001020f2, 0x00000000,
        0x00100e46, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_swapc4 = {ps_swapc4_code, sizeof(ps_swapc4_code)};
    static const DWORD ps_swapc5_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer cb0[3], immediateIndexed
        dcl_output o0.xyzw
        dcl_temps 2
        mov r1.xyzw, cb0[0].xyzw
        swapc r0.xyzw, r1.xyzw, r1.xyzw, cb0[1].xyzw, cb0[2].xyzw
        mov o0.xyzw, r1.xyzw
        ret
#endif
        0x43425844, 0x7078fb08, 0xdd24cd44, 0x469d3258, 0x9e33a0bc, 0x00000001, 0x0000010c, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000094, 0x00000050, 0x00000025,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000003, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000002, 0x06000036, 0x001000f2, 0x00000001, 0x00208e46, 0x00000000, 0x00000000,
        0x0d00008e, 0x001000f2, 0x00000000, 0x001000f2, 0x00000001, 0x00100e46, 0x00000001, 0x00208e46,
        0x00000000, 0x00000001, 0x00208e46, 0x00000000, 0x00000002, 0x05000036, 0x001020f2, 0x00000000,
        0x00100e46, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_swapc5 = {ps_swapc5_code, sizeof(ps_swapc5_code)};
    static const struct
    {
        const D3D12_SHADER_BYTECODE *ps;
        struct
        {
            struct vec4 src0;
            struct vec4 src1;
            struct vec4 src2;
        } input;
        union
        {
            struct vec4 f;
            struct uvec4 u;
            struct ivec4 i;
        } output;
    }
    tests[] =
    {
        {&ps_div, {{ 2.0f}, { 4.0f}}, {{     0.5f}}},
        {&ps_div, {{ 2.0f}, {-4.0f}}, {{    -0.5f}}},
        {&ps_div, {{-2.0f}, { 4.0f}}, {{    -0.5f}}},
        {&ps_div, {{-2.0f}, {-4.0f}}, {{     0.5f}}},
        {&ps_div, {{ 0.0f}, { 1.0f}}, {{     0.0f}}},
        {&ps_div, {{ 0.0f}, {-1.0f}}, {{    -0.0f}}},
        {&ps_div, {{ 1.0f}, { 0.0f}}, {{ INFINITY}}},
        {&ps_div, {{ 1.0f}, {-0.0f}}, {{-INFINITY}}},
        {&ps_div, {{-1.0f}, { 0.0f}}, {{-INFINITY}}},
        {&ps_div, {{-1.0f}, {-0.0f}}, {{ INFINITY}}},

        {&ps_dot2, {{1.0f, 1.0f}, {1.0f, 1.0f}}, {{2.0f}}},
        {&ps_dot2, {{1.0f, 1.0f}, {2.0f, 3.0f}}, {{5.0f}}},

        {&ps_eq, {{0.0f}, {0.0f}}, {.u = {0xffffffff}}},
        {&ps_eq, {{1.0f}, {0.0f}}, {.u = {0x00000000}}},
        {&ps_eq, {{0.0f}, {1.0f}}, {.u = {0x00000000}}},
        {&ps_eq, {{1.0f}, {1.0f}}, {.u = {0xffffffff}}},
        {&ps_eq, {{0.0f},  {NAN}}, {.u = {0x00000000}}},
        {&ps_eq, {{1.0f},  {NAN}}, {.u = {0x00000000}}},
        {&ps_eq, { {NAN},  {NAN}}, {.u = {0x00000000}}},

        {&ps_ne, {{0.0f}, {0.0f}}, {.u = {0x00000000}}},
        {&ps_ne, {{1.0f}, {0.0f}}, {.u = {0xffffffff}}},
        {&ps_ne, {{0.0f}, {1.0f}}, {.u = {0xffffffff}}},
        {&ps_ne, {{1.0f}, {1.0f}}, {.u = {0x00000000}}},
        {&ps_ne, {{0.0f},  {NAN}}, {.u = {0xffffffff}}},
        {&ps_ne, {{1.0f},  {NAN}}, {.u = {0xffffffff}}},
        {&ps_ne, { {NAN},  {NAN}}, {.u = {0xffffffff}}},

        {&ps_if, {{0.0f}}, {{1.0f, 0.0f, 0.0f, 1.0f}}},
        {&ps_if, {{1.0f}}, {{0.0f, 1.0f, 0.0f, 1.0f}}},

        {&ps_if_return, {{0.0f, 0.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{ NAN, 0.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{3.0f, 0.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 0.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f,  NAN, 0.0f, 0.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 3.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 0.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f,  NAN, 0.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 3.0f, 0.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 4.0f, 0.0f}}, {{1.0f, 1.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 5.0f, 0.0f}}, {{1.0f, 1.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f,  NAN}}, {{1.0f, 1.0f, 1.0f, 1.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 1.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 2.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 3.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 4.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 5.0f}}, {{1.0f, 1.0f, 1.0f, 1.0f}}},
        {&ps_if_return, {{5.0f, 4.0f, 0.0f, 5.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{ NAN,  NAN,  NAN,  NAN}}, {{1.0f, 1.0f, 1.0f, 1.0f}}},

        {&ps_nested_if, {{0.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, 0.0f, 1.0f}}},
        {&ps_nested_if, {{0.0f, 0.0f, 1.0f}}, {{1.0f, 0.0f, 0.0f, 1.0f}}},
        {&ps_nested_if, {{1.0f, 0.0f, 1.0f}}, {{0.0f, 0.0f, 1.0f, 1.0f}}},
        {&ps_nested_if, {{1.0f, 1.0f, 1.0f}}, {{0.0f, 1.0f, 0.0f, 1.0f}}},

        {&ps_loop_break, {{0.0f, 0.0f}}, {{0.0f}}},
        {&ps_loop_break, {{1.0f, 0.0f}}, {{1.0f}}},
        {&ps_loop_break, {{1.0f, 1.0f}}, {{1.0f}}},
        {&ps_loop_break, {{1.0f, 2.0f}}, {{1.0f}}},
        {&ps_loop_break, {{1.0f, 3.0f}}, {{1.0f}}},
        {&ps_loop_break, {{7.0f, 0.0f}}, {{1.0f}}},
        {&ps_loop_break, {{7.0f, 2.0f}}, {{1.0f}}},
        {&ps_loop_break, {{7.0f, 6.0f}}, {{1.0f}}},
        {&ps_loop_break, {{7.0f, 7.0f}}, {{7.0f}}},
        {&ps_loop_break, {{7.0f, 8.0f}}, {{7.0f}}},
        {&ps_loop_break, {{7.0f, 9.0f}}, {{7.0f}}},

        {&ps_loop_ret, {{0.0f, 0.0f}}, {{0.0f}}},
        {&ps_loop_ret, {{1.0f, 9.0f}}, {{1.0f}}},
        {&ps_loop_ret, {{2.0f, 2.0f}}, {{2.0f}}},
        {&ps_loop_ret, {{5.0f, 9.0f}}, {{5.0f}}},
        {&ps_loop_ret, {{1.0f, 0.0f}}, {{1.0f, 1.0f, 1.0f, 1.0f}}},
        {&ps_loop_ret, {{2.0f, 1.0f}}, {{1.0f, 1.0f, 1.0f, 1.0f}}},
        {&ps_loop_ret, {{8.0f, 7.0f}}, {{1.0f, 1.0f, 1.0f, 1.0f}}},

        {&ps_breakc_nz, {}, {{0.0f, 1.0f, 0.0f, 1.0f}}},
        {&ps_breakc_z,  {}, {{0.0f, 1.0f, 0.0f, 1.0f}}},

        {&ps_src_modifiers, {{ 1.0f,  1.0f,  1.0f,  2.0f}}, {{-1.0f, 1.0f, -1.0f, -2.0f}}},
        {&ps_src_modifiers, {{-1.0f, -1.0f, -1.0f, -2.0f}}, {{ 1.0f, 1.0f, -1.0f, -2.0f}}},

        {&ps_sat, {{ 0.0f,  1.0f,     2.0f,      3.0f}}, {{0.0f, 1.0f, 1.0f, 1.0f}}},
        {&ps_sat, {{-0.0f, -1.0f,    -2.0f,     -3.0f}}, {{0.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_sat, {{  NAN,  -NAN, INFINITY, -INFINITY}}, {{0.0f, 0.0f, 1.0f, 0.0f}}},

        {&ps_min_max, {{0.0f}, {     1.0f}}, {{     0.0f,     1.0f}}},
        {&ps_min_max, {{0.0f}, {    -1.0f}}, {{    -1.0f,     0.0f}}},
        {&ps_min_max, {{ NAN}, {     1.0f}}, {{     1.0f,     1.0f}}},
        {&ps_min_max, {{0.0f}, {      NAN}}, {{     0.0f,     0.0f}}},
        {&ps_min_max, {{0.0f}, { INFINITY}}, {{     0.0f, INFINITY}}},
        {&ps_min_max, {{1.0f}, { INFINITY}}, {{     1.0f, INFINITY}}},
        {&ps_min_max, {{0.0f}, {-INFINITY}}, {{-INFINITY,     0.0f}}},
        {&ps_min_max, {{1.0f}, {-INFINITY}}, {{-INFINITY,     1.0f}}},

        {&ps_ftou, {{     -NAN}}, {.u = { 0,  0 }}},
        {&ps_ftou, {{      NAN}}, {.u = { 0,  0 }}},
        {&ps_ftou, {{-INFINITY}}, {.u = { 0, ~0u}}},
        {&ps_ftou, {{ INFINITY}}, {.u = {~0u, 0 }}},
        {&ps_ftou, {{    -1.0f}}, {.u = { 0,  1 }}},
        {&ps_ftou, {{     1.0f}}, {.u = { 1,  0 }}},

        {&ps_ftoi, {{     -NAN}}, {.u = {      0,       0}}},
        {&ps_ftoi, {{      NAN}}, {.u = {      0,       0}}},
        {&ps_ftoi, {{-INFINITY}}, {.u = {INT_MIN, INT_MAX}}},
        {&ps_ftoi, {{ INFINITY}}, {.i = {INT_MAX, INT_MIN}}},
        {&ps_ftoi, {{    -1.0f}}, {.i = {     -1,       1}}},
        {&ps_ftoi, {{     1.0f}}, {.i = {      1,      -1}}},

        {&ps_round, {{    -0.5f}}, {{    -1.0f,      0.0f,     -0.0f}}},
        {&ps_round, {{    -0.0f}}, {{    -0.0f,     -0.0f,     -0.0f}}},
        {&ps_round, {{     0.0f}}, {{     0.0f,      0.0f,      0.0f}}},
        {&ps_round, {{     0.5f}}, {{     0.0f,      1.0f,      0.0f}}},
        {&ps_round, {{     3.0f}}, {{     3.0f,      3.0f,      3.0f}}},
        {&ps_round, {{ INFINITY}}, {{ INFINITY,  INFINITY,  INFINITY}}},
        {&ps_round, {{-INFINITY}}, {{-INFINITY, -INFINITY, -INFINITY}}},

        {&ps_frc, {{ 0.0f}}, {{0.0f, 0.0f}}},
        {&ps_frc, {{-0.0f}}, {{0.0f, 0.0f}}},
        {&ps_frc, {{ 1.0f}}, {{0.0f, 0.0f}}},
        {&ps_frc, {{-1.0f}}, {{0.0f, 0.0f}}},
        {&ps_frc, {{ 0.5f}}, {{0.5f, 0.5f}}},
        {&ps_frc, {{-0.5f}}, {{0.5f, 0.5f}}},

        {&ps_exp, {{     0.0f}}, {{   1.00f}}},
        {&ps_exp, {{    -0.0f}}, {{   1.00f}}},
        {&ps_exp, {{     2.0f}}, {{   4.00f}}},
        {&ps_exp, {{    -2.0f}}, {{   0.25f}}},
        {&ps_exp, {{ INFINITY}}, {{INFINITY}}},
        {&ps_exp, {{-INFINITY}}, {{   0.00f}}},

        {&ps_log, {{  -0.00f}}, {{-INFINITY}}},
        {&ps_log, {{   0.00f}}, {{-INFINITY}}},
        {&ps_log, {{INFINITY}}, {{ INFINITY}}},
        {&ps_log, {{   0.25f}}, {{    -2.0f}}},
        {&ps_log, {{   0.50f}}, {{    -1.0f}}},
        {&ps_log, {{   2.00f}}, {{     1.0f}}},
        {&ps_log, {{   8.00f}}, {{     3.0f}}},

        {&ps_rcp, {{-INFINITY}}, {{    -0.0f}}},
        {&ps_rcp, {{ INFINITY}}, {{     0.0f}}},
        {&ps_rcp, {{    -0.0f}}, {{-INFINITY}}},
        {&ps_rcp, {{     0.0f}}, {{ INFINITY}}},
        {&ps_rcp, {{    -1.0f}}, {{    -1.0f}}},
        {&ps_rcp, {{     1.0f}}, {{     1.0f}}},
        {&ps_rcp, {{    -2.0f}}, {{    -0.5f}}},
        {&ps_rcp, {{     2.0f}}, {{     0.5f}}},
    };

    static const struct
    {
        const D3D12_SHADER_BYTECODE *ps;
        union
        {
            struct
            {
                struct uvec4 src0;
                struct uvec4 src1;
                struct uvec4 src2;
            } u;
            struct
            {
                struct ivec4 src0;
                struct ivec4 src1;
                struct ivec4 src2;
            } i;
            struct
            {
                struct vec4 src0;
                struct vec4 src1;
                struct vec4 src2;
            } f;
        } input;
        union
        {
            struct uvec4 u;
            struct ivec4 i;
            struct vec4 f;
        } output;
        bool skip_on_warp;
    }
    uint_tests[] =
    {
        {&ps_bfi, {{{     0,      0,    0,    0}}}, {{         0,          0,          0,          0}}},
        {&ps_bfi, {{{     0,      0,    0,    1}}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{{   ~0u,      0,  ~0u,    0}}}, {{0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff}}},
        {&ps_bfi, {{{   ~0u,    ~0u,  ~0u,    0}}}, {{0x80000000, 0x80000000, 0x80000000, 0x80000000}}},
        {&ps_bfi, {{{   ~0u,  0x1fu,  ~0u,    0}}}, {{0x80000000, 0x80000000, 0x80000000, 0x80000000}}},
        {&ps_bfi, {{{   ~0u, ~0x1fu,  ~0u,    0}}}, {{0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff}}},
        {&ps_bfi, {{{     0,      0, 0xff,    1}}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{{     0,      0, 0xff,    2}}}, {{         2,          2,          2,          2}}},
        {&ps_bfi, {{{    16,     16, 0xff, 0xff}}}, {{0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff}}},
        {&ps_bfi, {{{     0,      0,  ~0u,  ~0u}}}, {{       ~0u,        ~0u,        ~0u,        ~0u}}},
        {&ps_bfi, {{{~0x1fu,      0,  ~0u,    0}}}, {{         0,          0,          0,          0}}},
        {&ps_bfi, {{{~0x1fu,      0,  ~0u,    1}}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{{~0x1fu,      0,  ~0u,    2}}}, {{         2,          2,          2,          2}}},
        {&ps_bfi, {{{     0, ~0x1fu,  ~0u,    0}}}, {{         0,          0,          0,          0}}},
        {&ps_bfi, {{{     0, ~0x1fu,  ~0u,    1}}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{{     0, ~0x1fu,  ~0u,    2}}}, {{         2,          2,          2,          2}}},
        {&ps_bfi, {{{~0x1fu, ~0x1fu,  ~0u,    0}}}, {{         0,          0,          0,          0}}},
        {&ps_bfi, {{{~0x1fu, ~0x1fu,  ~0u,    1}}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{{~0x1fu, ~0x1fu,  ~0u,    2}}}, {{         2,          2,          2,          2}}},

        {&ps_ibfe, {{{ 0,  4, 0x00000000}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{{ 0,  4, 0xffffffff}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{{ 0,  4, 0x7fffffff}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{{ 4,  0, 0x00000000}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{{ 4,  0, 0xfffffffa}}}, {{0xfffffffa, 0xfffffffa, 0xfffffffa, 0xfffffffa}}},
        {&ps_ibfe, {{{ 4,  0, 0x7ffffffc}}}, {{0xfffffffc, 0xfffffffc, 0xfffffffc, 0xfffffffc}}},
        {&ps_ibfe, {{{ 4,  4, 0x00000000}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{{ 4,  4, 0xffffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{ 4,  4, 0xffffff1f}}}, {{0x00000001, 0x00000001, 0x00000001, 0x00000001}}},
        {&ps_ibfe, {{{ 4,  4, 0x7fffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{23,  8, 0x00000000}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{{23,  8, 0xffffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{23,  8, 0x7fffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{30,  1, 0x00000000}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{{30,  1, 0xffffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{30,  1, 0x7fffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{15, 15, 0x7fffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{15, 15, 0x3fffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{15, 15, 0x1fffffff}}}, {{0x00003fff, 0x00003fff, 0x00003fff, 0x00003fff}}},
        {&ps_ibfe, {{{15, 15, 0xffff00ff}}}, {{0xfffffffe, 0xfffffffe, 0xfffffffe, 0xfffffffe}}},
        {&ps_ibfe, {{{16, 15, 0xffffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{16, 15, 0x3fffffff}}}, {{0x00007fff, 0x00007fff, 0x00007fff, 0x00007fff}}},
        {&ps_ibfe, {{{20, 15, 0xffffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{31, 31, 0xffffffff}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{31, 31, 0x80000000}}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{{31, 31, 0x7fffffff}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},

        {&ps_ubfe, {{{0x00000000}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ubfe, {{{0xffffffff}}}, {{0x0000000f, 0x007fffff, 0x0000007f, 0x3fffffff}}},
        {&ps_ubfe, {{{0xff000000}}}, {{0x00000000, 0x007f0000, 0x00000000, 0x3f800000}}},
        {&ps_ubfe, {{{0x00ff0000}}}, {{0x00000000, 0x0000ff00, 0x00000000, 0x007f8000}}},
        {&ps_ubfe, {{{0x000000ff}}}, {{0x0000000f, 0x00000000, 0x0000007f, 0x0000007f}}},
        {&ps_ubfe, {{{0x80000001}}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ubfe, {{{0xc0000003}}}, {{0x00000000, 0x00400000, 0x00000001, 0x20000001}}},

        {&ps_bfrev, {{{0x12345678}}}, {{0x1e6a2c48, 0x12345678, 0x1e6a0000, 0x2c480000}}},
        {&ps_bfrev, {{{0xffff0000}}}, {{0x0000ffff, 0xffff0000, 0x00000000, 0xffff0000}}},
        {&ps_bfrev, {{{0xffffffff}}}, {{0xffffffff, 0xffffffff, 0xffff0000, 0xffff0000}}},

        {&ps_bits, {{{         0,          0}}}, {{ 0, ~0u, ~0u, ~0u}}},
        {&ps_bits, {{{       ~0u,        ~0u}}}, {{32,   0,  31, ~0u}}},
        {&ps_bits, {{{0x7fffffff, 0x7fffffff}}}, {{31,   0,  30,  30}}},
        {&ps_bits, {{{0x80000000, 0x80000000}}}, {{ 1,  31,  31,  30}}},
        {&ps_bits, {{{0x00000001, 0x00000001}}}, {{ 1,   0,   0,   0}}},
        {&ps_bits, {{{0x80000001, 0x80000001}}}, {{ 2,   0,  31,  30}}},
        {&ps_bits, {{{0x88888888, 0x88888888}}}, {{ 8,   3,  31,  30}}},
        {&ps_bits, {{{0xcccccccc, 0xcccccccc}}}, {{16,   2,  31,  29}}},
        {&ps_bits, {{{0x11111111, 0x11111c11}}}, {{ 8,   0,  28,  28}}},
        {&ps_bits, {{{0x0000000f, 0x0000000f}}}, {{ 4,   0,   3,   3}}},
        {&ps_bits, {{{0x8000000f, 0x8000000f}}}, {{ 5,   0,  31,  30}}},
        {&ps_bits, {{{0x00080000, 0x00080000}}}, {{ 1,  19,  19,  19}}},

        {&ps_ishr, {{{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {~0x1fu, 0, 32, 64}}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ishr, {{{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}, {~0x1fu, 0, 32, 64}}},
                   {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ishr, {{{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}, {~0x1fu, 0, 32, 64}}},
                   {{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}}},
        {&ps_ishr, {{{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {    31, 7, 15, 11}}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ishr, {{{0x80000000, 0x80000000, 0x80000000, 0x80000000}, {    31, 7, 15, 11}}},
                   {{0xffffffff, 0xff000000, 0xffff0000, 0xfff00000}}},

        {&ps_ushr, {{{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {~0x1fu, 0, 32, 64}}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ushr, {{{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}, {~0x1fu, 0, 32, 64}}},
                   {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ushr, {{{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}, {~0x1fu, 0, 32, 64}}},
                   {{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}}},
        {&ps_ushr, {{{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {    31, 7, 15, 11}}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ushr, {{{0x80000000, 0x80000000, 0x80000000, 0x80000000}, {    31, 7, 15, 11}}},
                   {{0x00000001, 0x01000000, 0x00010000, 0x00100000}}},

        {&ps_ishl, {{{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {~0x1fu, 0, 32, 64}}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ishl, {{{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}, {~0x1fu, 0, 32, 64}}},
                   {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ishl, {{{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}, {~0x1fu, 0, 32, 64}}},
                   {{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}}},
        {&ps_ishl, {{{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {    31, 7, 15, 11}}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ishl, {{{0x80000000, 0x80000000, 0x80000000, 0x80000000}, {    31, 7, 15, 11}}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ishl, {{{0x00000001, 0x00000001, 0x00000001, 0x800feac1}, {    31, 7, 15, 11}}},
                   {{0x80000000, 0x00000080, 0x00008000, 0x7f560800}}},

        {&ps_not, {{{0x00000000, 0xffffffff}}}, {{0xffffffff, 0x00000000, 0x00000000, 0xffffffff}}},
        {&ps_not, {{{0xf0f0f0f0, 0x0f0f0f0f}}}, {{0x0f0f0f0f, 0xf0f0f0f0, 0xf0f0f0f0, 0x0f0f0f0f}}},

        {&ps_icmp, {.i = {{ 0,  0}}}, {{~0u, ~0u,  0,   0}}},
        {&ps_icmp, {.i = {{ 1,  0}}}, {{ 0,  ~0u,  0,  ~0u}}},
        {&ps_icmp, {.i = {{ 0,  1}}}, {{ 0,   0,  ~0u, ~0u}}},
        {&ps_icmp, {.i = {{ 1,  1}}}, {{~0u, ~0u,  0,   0}}},
        {&ps_icmp, {.i = {{-1, -1}}}, {{~0u, ~0u,  0,   0}}},
        {&ps_icmp, {.i = {{ 0, -1}}}, {{ 0,  ~0u,  0,  ~0u}}},
        {&ps_icmp, {.i = {{-1,  0}}}, {{ 0,   0,  ~0u, ~0u}}},
        {&ps_icmp, {.i = {{ 1, -1}}}, {{ 0,  ~0u,  0,  ~0u}}},
        {&ps_icmp, {.i = {{-1,  1}}}, {{ 0,   0,  ~0u, ~0u}}},
        {&ps_icmp, {.i = {{-2, -1}}}, {{ 0,   0,  ~0u, ~0u}}},

        {&ps_ucmp, {{{0,  0}}}, {{~0u,  0, }}},
        {&ps_ucmp, {{{1,  0}}}, {{~0u,  0, }}},
        {&ps_ucmp, {{{0,  1}}}, {{ 0,  ~0u,}}},
        {&ps_ucmp, {{{1,  1}}}, {{~0u,  0, }}},
        {&ps_ucmp, {{{1,  2}}}, {{ 0,  ~0u,}}},

        {&ps_umin_umax, {{{ 0,   0}}},  {{ 0,   0}}},
        {&ps_umin_umax, {{{ 0,   1}}},  {{ 0,   1}}},
        {&ps_umin_umax, {{{ 1,   0}}},  {{ 0,   1}}},
        {&ps_umin_umax, {{{~0u, ~0u}}}, {{~0u, ~0u}}},
        {&ps_umin_umax, {{{ 0,  ~0u}}}, {{ 0,  ~0u}}},
        {&ps_umin_umax, {{{~0u,  0}}},  {{ 0,  ~0u}}},

        {&ps_f16tof32, {{{0x00000000, 0x00003c00, 0x00005640, 0x00005bd0}}}, {{0, 1, 100, 250}}},
        {&ps_f16tof32, {{{0x00010000, 0x00013c00, 0x00015640, 0x00015bd0}}}, {{0, 1, 100, 250}}},
        {&ps_f16tof32, {{{0x000f0000, 0x000f3c00, 0x000f5640, 0x000f5bd0}}}, {{0, 1, 100, 250}}},
        {&ps_f16tof32, {{{0xffff0000, 0xffff3c00, 0xffff5640, 0xffff5bd0}}}, {{0, 1, 100, 250}}},

        {&ps_f32tof16, {.f = {{0.0f, 1.0f, -1.0f, 666.0f}}}, {{0, 0x3c00, 0xbc00, 0x6134}}},

        {&ps_imad, {{{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}}, {{ 0,  0,  0,  0}}},
        {&ps_imad, {{{0, 0, 0, 0}, {0, 0, 0, 0}, {1, 2, 0, 0}}}, {{ 1,  2,  0,  0}}},
        {&ps_imad, {{{2, 3, 4, 5}, {5, 5, 5, 5}, {0, 0, 0, 0}}}, {{10, 15, 20, 25}}},
        {&ps_imad, {{{2, 3, 4, 5}, {5, 5, 5, 5}, {5, 5, 6, 6}}}, {{15, 20, 14, 19}}},

        {&ps_imul, {{{0}, { 0u}}}, {{ 0u}}},
        {&ps_imul, {{{1}, { 2u}}}, {{ 2u}}},
        {&ps_imul, {{{1}, { 3u}}}, {{ 3u}}},
        {&ps_imul, {{{6}, { 3u}}}, {{18u}}},
        {&ps_imul, {{{1}, {~0u}}}, {{~0u}}},
        {&ps_imul, {{{2}, {~0u}}}, {{~1u}}},
        {&ps_imul, {{{3}, {~0u}}}, {{~2u}}},

        {&ps_udiv, {{{0}, {0}}}, {{~0u, ~0u}}},
        {&ps_udiv, {{{1}, {0}}}, {{~0u, ~0u}}},
        {&ps_udiv, {{{1}, {1}}}, {{ 1u,  0u}}},
        {&ps_udiv, {{{7}, {1}}}, {{ 7u,  0u}}},
        {&ps_udiv, {{{7}, {2}}}, {{ 3u,  1u}}},
        {&ps_udiv, {{{7}, {3}}}, {{ 2u,  1u}}},
        {&ps_udiv, {{{7}, {4}}}, {{ 1u,  3u}}},
        {&ps_udiv, {{{7}, {5}}}, {{ 1u,  2u}}},
        {&ps_udiv, {{{7}, {6}}}, {{ 1u,  1u}}},
        {&ps_udiv, {{{7}, {7}}}, {{ 1u,  0u}}},

        {&ps_nested_switch, {{{~0u, 0, 0, 0}, {0}}}, {{  1,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 0u, 0, 0, 0}, {0}}}, {{  2,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 1u, 0, 0, 0}, {0}}}, {{  2,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 2u, 0, 0, 0}, {0}}}, {{  2,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 0u, 0, 0, 0}, {1}}}, {{  0,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 1u, 0, 0, 0}, {2}}}, {{  0,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 2u, 0, 0, 0}, {3}}}, {{  0,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 3u, 0, 0, 0}, {0}}}, {{  0,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 3u, 0, 0, 0}, {1}}}, {{  0,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 5u, 1, 2, 3}, {0}}}, {{  0,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 6u, 1, 2, 3}, {1}}}, {{  0,   0,   0,   0}}},
        {&ps_nested_switch, {{{ 4u, 0, 0, 0}, {0}}}, {{128, 128, 128, 128}}},
        {&ps_nested_switch, {{{ 4u, 0, 0, 0}, {1}}}, {{  0,   0,   1,   1}}},
        {&ps_nested_switch, {{{ 4u, 1, 0, 0}, {1}}}, {{  0,   0,   1,   1}}},
        {&ps_nested_switch, {{{ 4u, 2, 0, 0}, {1}}}, {{  0,   0,   1,   1}}},
        {&ps_nested_switch, {{{ 4u, 3, 0, 0}, {1}}}, {{  0,   0,   1,   1}}},
        {&ps_nested_switch, {{{ 4u, 0, 0, 1}, {1}}}, {{255, 255, 255, 255}}},
        {&ps_nested_switch, {{{ 4u, 1, 0, 1}, {1}}}, {{255, 255, 255, 255}}},
        {&ps_nested_switch, {{{ 4u, 2, 0, 1}, {1}}}, {{255, 255, 255, 255}}},
        {&ps_nested_switch, {{{ 4u, 3, 0, 1}, {1}}}, {{255, 255, 255, 255}}},
        {&ps_nested_switch, {{{ 4u, 0, 1, 1}, {1}}}, {{  2,   2,   2,   2}}},
        {&ps_nested_switch, {{{ 4u, 1, 1, 1}, {1}}}, {{  2,   2,   2,   2}}},
        {&ps_nested_switch, {{{ 4u, 2, 1, 1}, {1}}}, {{  2,   2,   2,   2}}},
        {&ps_nested_switch, {{{ 4u, 3, 1, 1}, {1}}}, {{  2,   2,   2,   2}}},
        {&ps_nested_switch, {{{ 4u, 0, 3, 1}, {1}}}, {{  6,   6,   6,   6}}},
        {&ps_nested_switch, {{{ 4u, 1, 3, 1}, {1}}}, {{  6,   6,   6,   6}}},
        {&ps_nested_switch, {{{ 4u, 2, 3, 1}, {1}}}, {{  6,   6,   6,   6}}},
        {&ps_nested_switch, {{{ 4u, 3, 3, 1}, {1}}}, {{  6,   6,   6,   6}}},
        {&ps_nested_switch, {{{ 4u, 5, 3, 1}, {1}}}, {{  1,   1,   1,   1}}},
        {&ps_nested_switch, {{{ 4u, 6, 3, 1}, {1}}}, {{  1,   1,   1,   1}}},
        {&ps_nested_switch, {{{ 4u, 7, 3, 1}, {1}}}, {{  1,   1,   1,   1}}},
        {&ps_nested_switch, {{{ 4u, 8, 3, 1}, {1}}}, {{  1,   1,   1,   1}}},

        {&ps_switch_no_default, {{{0}}}, {{1, 1, 1, 1}}},
        {&ps_switch_no_default, {{{1}}}, {{3, 3, 3, 3}}},
        {&ps_switch_no_default, {{{2}}}, {{3, 3, 3, 3}}},
        {&ps_switch_no_default, {{{3}}}, {{2, 2, 2, 2}}},
        {&ps_switch_no_default, {{{4}}}, {{3, 3, 3, 3}}},

        {&ps_movc, {{{0, 0, 0, 0}, {1, 2, 3, 4}, {5, 6, 7, 8}}}, {{5, 6, 7, 8}}, true},
        {&ps_movc, {{{0, 0, 0, 1}, {1, 2, 3, 4}, {5, 6, 7, 8}}}, {{5, 6, 7, 4}}, true},
        {&ps_movc, {{{1, 0, 0, 0}, {1, 2, 3, 4}, {5, 6, 7, 8}}}, {{1, 6, 7, 8}}, true},
        {&ps_movc, {{{1, 0, 0, 1}, {1, 2, 3, 4}, {5, 6, 7, 8}}}, {{1, 6, 7, 4}}, true},
        {&ps_movc, {{{0, 1, 1, 0}, {1, 2, 3, 4}, {5, 6, 7, 8}}}, {{5, 2, 3, 8}}, true},
        {&ps_movc, {{{1, 1, 1, 1}, {1, 2, 3, 4}, {5, 6, 7, 8}}}, {{1, 2, 3, 4}}, true},

        {
            &ps_swapc0,
            {{{0, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc0,
            {{{1, 1, 1, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xdddd}},
        },
        {
            &ps_swapc0,
            {{{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
                    {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xdddd}},
        },
        {
            &ps_swapc0,
            {{{1, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xcccc, 0xeeee}},
        },
        {
            &ps_swapc0,
            {{{1, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xffff, 0xdddd}},
        },
        {
            &ps_swapc0,
            {{{1, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc0,
            {{{0, 1, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc0,
            {{{0, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xcccc, 0xeeee}},
        },
        {
            &ps_swapc0,
            {{{0, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xdddd}},
        },

        {
            &ps_swapc1,
            {{{0, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}
        },
        {
            &ps_swapc1,
            {{{1, 1, 1, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc1,
            {{{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
                    {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc1,
            {{{1, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xffff, 0xdddd}},
        },
        {
            &ps_swapc1,
            {{{1, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xcccc, 0xeeee}},
        },
        {
            &ps_swapc1,
            {{{1, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xcccc, 0xdddd}}
        },
        {
            &ps_swapc1,
            {{{0, 1, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xcccc, 0xdddd}}
        },
        {
            &ps_swapc1,
            {{{0, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xffff, 0xdddd}}
        },
        {
            &ps_swapc1,
            {{{0, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xeeee}},
        },

        {
            &ps_swapc2,
            {{{0, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc2,
            {{{1, 1, 1, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xdddd}},
        },
        {
            &ps_swapc2,
            {{{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
                    {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xdddd}},
        },
        {
            &ps_swapc2,
            {{{1, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xcccc, 0xeeee}},
        },
        {
            &ps_swapc2,
            {{{1, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xffff, 0xdddd}},
        },
        {
            &ps_swapc2,
            {{{1, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc2,
            {{{0, 1, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc2,
            {{{0, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xcccc, 0xeeee}},
        },
        {
            &ps_swapc2,
            {{{0, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xdddd}},
        },

        {
            &ps_swapc3,
            {{{0, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}
        },
        {
            &ps_swapc3,
            {{{1, 1, 1, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc3,
            {{{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
                    {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc3,
            {{{1, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xffff, 0xdddd}},
        },
        {
            &ps_swapc3,
            {{{1, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xcccc, 0xeeee}},
        },
        {
            &ps_swapc3,
            {{{1, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xcccc, 0xdddd}}
        },
        {
            &ps_swapc3,
            {{{0, 1, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xcccc, 0xdddd}}
        },
        {
            &ps_swapc3,
            {{{0, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xffff, 0xdddd}}
        },
        {
            &ps_swapc3,
            {{{0, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xeeee}},
        },

        {
            &ps_swapc4,
            {{{0, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc4,
            {{{1, 1, 1, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xdddd}},
        },
        {
            &ps_swapc4,
            {{{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
                    {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xdddd}},
        },
        {
            &ps_swapc4,
            {{{1, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xcccc, 0xeeee}},
        },
        {
            &ps_swapc4,
            {{{1, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xffff, 0xdddd}},
        },
        {
            &ps_swapc4,
            {{{1, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc4,
            {{{0, 1, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc4,
            {{{0, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xcccc, 0xeeee}},
        },
        {
            &ps_swapc4,
            {{{0, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xdddd}},
        },

        {
            &ps_swapc5,
            {{{0, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}
        },
        {
            &ps_swapc5,
            {{{1, 1, 1, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc5,
            {{{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
                    {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xc0de, 0xffff, 0xeeee}},
        },
        {
            &ps_swapc5,
            {{{1, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xffff, 0xdddd}},
        },
        {
            &ps_swapc5,
            {{{1, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xcccc, 0xeeee}},
        },
        {
            &ps_swapc5,
            {{{1, 0, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xdead, 0xbbbb, 0xcccc, 0xdddd}}
        },
        {
            &ps_swapc5,
            {{{0, 1, 0, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xc0de, 0xcccc, 0xdddd}}
        },
        {
            &ps_swapc5,
            {{{0, 0, 1, 0}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xffff, 0xdddd}}
        },
        {
            &ps_swapc5,
            {{{0, 0, 0, 1}, {0xdead, 0xc0de, 0xffff, 0xeeee}, {0xaaaa, 0xbbbb, 0xcccc, 0xdddd}}},
            {{0xaaaa, 0xbbbb, 0xcccc, 0xeeee}},
        },
    };

    assert(sizeof(tests->input) == sizeof(uint_tests->input));

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_cb_root_signature(context.device,
            0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    cb = create_upload_buffer(context.device, sizeof(tests->input), NULL);

    current_ps = NULL;
    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        if (current_ps != tests[i].ps)
        {
            if (context.pipeline_state)
                ID3D12PipelineState_Release(context.pipeline_state);
            current_ps = tests[i].ps;
            context.pipeline_state = create_pipeline_state(context.device,
                    context.root_signature, desc.rt_format, NULL, current_ps, NULL);
        }

        update_buffer_data(cb, 0, sizeof(tests[i].input), &tests[i].input);

        if (i)
            transition_resource_state(command_list, context.render_target,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list, 0,
                ID3D12Resource_GetGPUVirtualAddress(cb));
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        transition_resource_state(command_list, context.render_target,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check_sub_resource_vec4(context.render_target, 0, queue, command_list, &tests[i].output.f, 2);

        reset_command_list(command_list, context.allocator);
    }

    ID3D12Resource_Release(context.render_target);
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_UINT;
    create_render_target(&context, &desc, &context.render_target, &context.rtv);

    for (i = 0; i < ARRAY_SIZE(uint_tests); ++i)
    {
        if (uint_tests[i].skip_on_warp && use_warp_device)
        {
            skip("Skipping shader test on WARP.\n");
            continue;
        }

        if (current_ps != uint_tests[i].ps)
        {
            if (context.pipeline_state)
                ID3D12PipelineState_Release(context.pipeline_state);
            current_ps = uint_tests[i].ps;
            context.pipeline_state = create_pipeline_state(context.device,
                    context.root_signature, desc.rt_format, NULL, current_ps, NULL);
        }

        update_buffer_data(cb, 0, sizeof(uint_tests[i].input), &uint_tests[i].input);

        if (i)
            transition_resource_state(command_list, context.render_target,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list, 0,
                ID3D12Resource_GetGPUVirtualAddress(cb));
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        transition_resource_state(command_list, context.render_target,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
        for (y = 0; y < rb.height; ++y)
        {
            for (x = 0; x < rb.width; ++x)
            {
                const struct uvec4 *v = get_readback_uvec4(&rb, x, y);
                ok(compare_uvec4(v, &uint_tests[i].output.u),
                        "Got 0x%08x, 0x%08x, 0x%08x, 0x%08x expected 0x%08x, 0x%08x, 0x%08x, 0x%08x.\n",
                        v->x, v->y, v->z, v->w, uint_tests[i].output.u.x, uint_tests[i].output.u.y,
                        uint_tests[i].output.u.z, uint_tests[i].output.u.w);
            }
        }
        release_resource_readback(&rb);

        reset_command_list(command_list, context.allocator);
    }

    ID3D12Resource_Release(cb);
    destroy_test_context(&context);
}

static void test_shader_interstage_interface(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    D3D12_INPUT_LAYOUT_DESC input_layout;
    struct test_context_desc desc;
    D3D12_VERTEX_BUFFER_VIEW vbv;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Resource *vb;

    static const DWORD vs_code[] =
    {
#if 0
        struct vertex
        {
            float4 position : SV_Position;
            float2 t0 : TEXCOORD0;
            nointerpolation float t1 : TEXCOORD1;
            uint t2 : TEXCOORD2;
            uint t3 : TEXCOORD3;
            float t4 : TEXCOORD4;
        };

        void main(in vertex vin, out vertex vout)
        {
            vout = vin;
        }
#endif
        0x43425844, 0x561ea178, 0x7b8f454c, 0x69091b4f, 0xf28d9a01, 0x00000001, 0x000002c0, 0x00000003,
        0x0000002c, 0x000000e4, 0x0000019c, 0x4e475349, 0x000000b0, 0x00000006, 0x00000008, 0x00000098,
        0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x00000f0f, 0x000000a4, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000303, 0x000000a4, 0x00000001, 0x00000000, 0x00000003, 0x00000002,
        0x00000101, 0x000000a4, 0x00000002, 0x00000000, 0x00000001, 0x00000003, 0x00000101, 0x000000a4,
        0x00000003, 0x00000000, 0x00000001, 0x00000004, 0x00000101, 0x000000a4, 0x00000004, 0x00000000,
        0x00000003, 0x00000005, 0x00000101, 0x505f5653, 0x7469736f, 0x006e6f69, 0x43584554, 0x44524f4f,
        0xababab00, 0x4e47534f, 0x000000b0, 0x00000006, 0x00000008, 0x00000098, 0x00000000, 0x00000001,
        0x00000003, 0x00000000, 0x0000000f, 0x000000a4, 0x00000000, 0x00000000, 0x00000003, 0x00000001,
        0x00000c03, 0x000000a4, 0x00000004, 0x00000000, 0x00000003, 0x00000001, 0x00000b04, 0x000000a4,
        0x00000001, 0x00000000, 0x00000003, 0x00000002, 0x00000e01, 0x000000a4, 0x00000002, 0x00000000,
        0x00000001, 0x00000002, 0x00000d02, 0x000000a4, 0x00000003, 0x00000000, 0x00000001, 0x00000002,
        0x00000b04, 0x505f5653, 0x7469736f, 0x006e6f69, 0x43584554, 0x44524f4f, 0xababab00, 0x58454853,
        0x0000011c, 0x00010050, 0x00000047, 0x0100086a, 0x0300005f, 0x001010f2, 0x00000000, 0x0300005f,
        0x00101032, 0x00000001, 0x0300005f, 0x00101012, 0x00000002, 0x0300005f, 0x00101012, 0x00000003,
        0x0300005f, 0x00101012, 0x00000004, 0x0300005f, 0x00101012, 0x00000005, 0x04000067, 0x001020f2,
        0x00000000, 0x00000001, 0x03000065, 0x00102032, 0x00000001, 0x03000065, 0x00102042, 0x00000001,
        0x03000065, 0x00102012, 0x00000002, 0x03000065, 0x00102022, 0x00000002, 0x03000065, 0x00102042,
        0x00000002, 0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000000, 0x05000036, 0x00102032,
        0x00000001, 0x00101046, 0x00000001, 0x05000036, 0x00102042, 0x00000001, 0x0010100a, 0x00000005,
        0x05000036, 0x00102012, 0x00000002, 0x0010100a, 0x00000002, 0x05000036, 0x00102022, 0x00000002,
        0x0010100a, 0x00000003, 0x05000036, 0x00102042, 0x00000002, 0x0010100a, 0x00000004, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs = {vs_code, sizeof(vs_code)};
    static const DWORD ps_code[] =
    {
#if 0
        void main(float4 position : SV_Position, float2 t0 : TEXCOORD0,
                nointerpolation float t1 : TEXCOORD1, uint t2 : TEXCOORD2,
                uint t3 : TEXCOORD3, float t4 : TEXCOORD4, out float4 o : SV_Target)
        {
            o.x = t0.y + t1;
            o.y = t2 + t3;
            o.z = t4;
            o.w = t0.x;
        }
#endif
        0x43425844, 0x21076b15, 0x493d36f1, 0x0cd125d6, 0x1e92c724, 0x00000001, 0x000001e0, 0x00000003,
        0x0000002c, 0x000000e4, 0x00000118, 0x4e475349, 0x000000b0, 0x00000006, 0x00000008, 0x00000098,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x000000a4, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000303, 0x000000a4, 0x00000004, 0x00000000, 0x00000003, 0x00000001,
        0x00000404, 0x000000a4, 0x00000001, 0x00000000, 0x00000003, 0x00000002, 0x00000101, 0x000000a4,
        0x00000002, 0x00000000, 0x00000001, 0x00000002, 0x00000202, 0x000000a4, 0x00000003, 0x00000000,
        0x00000001, 0x00000002, 0x00000404, 0x505f5653, 0x7469736f, 0x006e6f69, 0x43584554, 0x44524f4f,
        0xababab00, 0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000,
        0x00000003, 0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000c0,
        0x00000050, 0x00000030, 0x0100086a, 0x03001062, 0x00101032, 0x00000001, 0x03001062, 0x00101042,
        0x00000001, 0x03000862, 0x00101012, 0x00000002, 0x03000862, 0x00101022, 0x00000002, 0x03000862,
        0x00101042, 0x00000002, 0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x0700001e,
        0x00100012, 0x00000000, 0x0010101a, 0x00000002, 0x0010102a, 0x00000002, 0x05000056, 0x00102022,
        0x00000000, 0x0010000a, 0x00000000, 0x07000000, 0x00102012, 0x00000000, 0x0010101a, 0x00000001,
        0x0010100a, 0x00000002, 0x05000036, 0x001020c2, 0x00000000, 0x001012a6, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const D3D12_INPUT_ELEMENT_DESC layout_desc[] =
    {
        {"SV_POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",    0, DXGI_FORMAT_R32G32_FLOAT, 0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",    1, DXGI_FORMAT_R32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",    2, DXGI_FORMAT_R32_UINT,     0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",    3, DXGI_FORMAT_R32_UINT,     0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",    4, DXGI_FORMAT_R32_FLOAT,    0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static const struct
    {
        struct vec2 position;
        struct vec2 t0;
        float t1;
        unsigned int t2;
        unsigned int t3;
        float t4;
    }
    quad[] =
    {
        {{-1.0f, -1.0f}, {3.0f, 5.0f}, 5.0f, 2, 6, 7.0f},
        {{-1.0f,  1.0f}, {3.0f, 5.0f}, 5.0f, 2, 6, 7.0f},
        {{ 1.0f, -1.0f}, {3.0f, 5.0f}, 5.0f, 2, 6, 7.0f},
        {{ 1.0f,  1.0f}, {3.0f, 5.0f}, 5.0f, 2, 6, 7.0f},
    };
    static const struct vec4 expected_result = {10.0f, 8.0f, 7.0f, 3.0f};

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_empty_root_signature(context.device,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    input_layout.pInputElementDescs = layout_desc;
    input_layout.NumElements = ARRAY_SIZE(layout_desc);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, &vs, &ps, &input_layout);

    vb = create_upload_buffer(context.device, sizeof(quad), quad);

    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb);
    vbv.StrideInBytes = sizeof(*quad);
    vbv.SizeInBytes = sizeof(quad);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 4, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_vec4(context.render_target, 0, queue, command_list, &expected_result, 0);

    ID3D12Resource_Release(vb);
    destroy_test_context(&context);
}

static void check_descriptor_range_(unsigned int line, const D3D12_DESCRIPTOR_RANGE *range,
        const D3D12_DESCRIPTOR_RANGE *expected_range)
{
    ok_(line)(range->RangeType == expected_range->RangeType,
            "Got range type %#x, expected %#x.\n", range->RangeType, expected_range->RangeType);
    ok_(line)(range->NumDescriptors == expected_range->NumDescriptors,
            "Got descriptor count %u, expected %u.\n", range->NumDescriptors, expected_range->NumDescriptors);
    ok_(line)(range->BaseShaderRegister == expected_range->BaseShaderRegister,
            "Got base shader register %u, expected %u.\n",
            range->BaseShaderRegister, expected_range->BaseShaderRegister);
    ok_(line)(range->RegisterSpace == expected_range->RegisterSpace,
            "Got register space %u, expected %u.\n", range->RegisterSpace, expected_range->RegisterSpace);
    ok_(line)(range->OffsetInDescriptorsFromTableStart == expected_range->OffsetInDescriptorsFromTableStart,
            "Got offset %u, expected %u.\n", range->OffsetInDescriptorsFromTableStart,
            expected_range->OffsetInDescriptorsFromTableStart);
}

static void check_root_parameter_(unsigned int line, const D3D12_ROOT_PARAMETER *parameter,
        const D3D12_ROOT_PARAMETER *expected_parameter)
{
    const D3D12_ROOT_DESCRIPTOR *descriptor, *expected_descriptor;
    const D3D12_ROOT_DESCRIPTOR_TABLE *table, *expected_table;
    const D3D12_ROOT_CONSTANTS *constants, *expected_constants;
    unsigned int i;

    ok_(line)(parameter->ParameterType == expected_parameter->ParameterType,
            "Got type %#x, expected %#x.\n", parameter->ParameterType, expected_parameter->ParameterType);
    if (parameter->ParameterType != expected_parameter->ParameterType)
        return;

    switch (parameter->ParameterType)
    {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            table = &parameter->DescriptorTable;
            expected_table = &expected_parameter->DescriptorTable;
            ok_(line)(table->NumDescriptorRanges == expected_table->NumDescriptorRanges,
                    "Got range count %u, expected %u.\n",
                    table->NumDescriptorRanges, expected_table->NumDescriptorRanges);
            if (table->NumDescriptorRanges == expected_table->NumDescriptorRanges)
            {
                for (i = 0; i < table->NumDescriptorRanges; ++i)
                    check_descriptor_range_(line, &table->pDescriptorRanges[i],
                            &expected_table->pDescriptorRanges[i]);
            }
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            constants = &parameter->Constants;
            expected_constants = &expected_parameter->Constants;
            ok_(line)(constants->ShaderRegister == expected_constants->ShaderRegister,
                    "Got shader register %u, expected %u.\n",
                    constants->ShaderRegister, expected_constants->ShaderRegister);
            ok_(line)(constants->RegisterSpace == expected_constants->RegisterSpace,
                    "Got register space %u, expected %u.\n",
                    constants->RegisterSpace, expected_constants->RegisterSpace);
            ok_(line)(constants->Num32BitValues == expected_constants->Num32BitValues,
                    "Got 32-bit value count %u, expected %u.\n",
                    constants->Num32BitValues, expected_constants->Num32BitValues);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            descriptor = &parameter->Descriptor;
            expected_descriptor = &expected_parameter->Descriptor;
            ok_(line)(descriptor->ShaderRegister == expected_descriptor->ShaderRegister,
                    "Got shader register %u, expected %u.\n",
                    descriptor->ShaderRegister, expected_descriptor->ShaderRegister);
            ok_(line)(descriptor->RegisterSpace == expected_descriptor->RegisterSpace,
                    "Got register space %u, expected %u.\n",
                    descriptor->RegisterSpace, expected_descriptor->RegisterSpace);
            break;
        default:
            trace("Unhandled type %#x.\n", parameter->ParameterType);
    }

    ok_(line)(parameter->ShaderVisibility == expected_parameter->ShaderVisibility,
            "Got shader visibility %#x, expected %#x.\n",
            parameter->ShaderVisibility, expected_parameter->ShaderVisibility);
}

static void check_static_sampler_(unsigned int line, const D3D12_STATIC_SAMPLER_DESC *sampler,
        const D3D12_STATIC_SAMPLER_DESC *expected_sampler)
{
    ok_(line)(sampler->Filter == expected_sampler->Filter,
            "Got filter %#x, expected %#x.\n", sampler->Filter, expected_sampler->Filter);
    ok_(line)(sampler->AddressU == expected_sampler->AddressU,
            "Got address U %#x, expected %#x.\n", sampler->AddressU, expected_sampler->AddressU);
    ok_(line)(sampler->AddressV == expected_sampler->AddressV,
            "Got address V %#x, expected %#x.\n", sampler->AddressV, expected_sampler->AddressV);
    ok_(line)(sampler->AddressW == expected_sampler->AddressW,
            "Got address W %#x, expected %#x.\n", sampler->AddressW, expected_sampler->AddressW);
    ok_(line)(sampler->MipLODBias == expected_sampler->MipLODBias,
            "Got mip LOD bias %.8e, expected %.8e.\n", sampler->MipLODBias, expected_sampler->MipLODBias);
    ok_(line)(sampler->MaxAnisotropy == expected_sampler->MaxAnisotropy,
            "Got max anisotropy %u, expected %u.\n", sampler->MaxAnisotropy, expected_sampler->MaxAnisotropy);
    ok_(line)(sampler->ComparisonFunc == expected_sampler->ComparisonFunc,
            "Got comparison func %#x, expected %#x.\n", sampler->ComparisonFunc, expected_sampler->ComparisonFunc);
    ok_(line)(sampler->BorderColor == expected_sampler->BorderColor,
            "Got border color %#x, expected %#x.\n", sampler->BorderColor, expected_sampler->BorderColor);
    ok_(line)(sampler->MinLOD == expected_sampler->MinLOD,
            "Got min LOD %.8e, expected %.8e.\n", sampler->MinLOD, expected_sampler->MinLOD);
    ok_(line)(sampler->MaxLOD == expected_sampler->MaxLOD,
            "Got max LOD %.8e, expected %.8e.\n", sampler->MaxLOD, expected_sampler->MaxLOD);
    ok_(line)(sampler->ShaderRegister == expected_sampler->ShaderRegister,
            "Got shader register %u, expected %u.\n", sampler->ShaderRegister, expected_sampler->ShaderRegister);
    ok_(line)(sampler->RegisterSpace == expected_sampler->RegisterSpace,
            "Got register space %u, expected %u.\n", sampler->RegisterSpace, expected_sampler->RegisterSpace);
    ok_(line)(sampler->ShaderVisibility == expected_sampler->ShaderVisibility,
            "Got shader visibility %#x, expected %#x.\n",
            sampler->ShaderVisibility, expected_sampler->ShaderVisibility);
}

#define check_root_signature_desc(desc, expected) check_root_signature_desc_(__LINE__, desc, expected)
static void check_root_signature_desc_(unsigned int line, const D3D12_ROOT_SIGNATURE_DESC *desc,
        const D3D12_ROOT_SIGNATURE_DESC *expected_desc)
{
    unsigned int i;

    ok_(line)(desc->NumParameters == expected_desc->NumParameters,
            "Got parameter count %u, expected %u.\n",
            desc->NumParameters, expected_desc->NumParameters);
    if (!expected_desc->pParameters)
    {
        ok_(line)(!desc->pParameters, "Got unexpected parameters %p.\n", desc->pParameters);
    }
    else if (desc->NumParameters == expected_desc->NumParameters)
    {
        for (i = 0; i < desc->NumParameters; ++i)
            check_root_parameter_(line, &desc->pParameters[i], &expected_desc->pParameters[i]);
    }
    ok_(line)(desc->NumStaticSamplers == expected_desc->NumStaticSamplers,
            "Got static sampler count %u, expected %u.\n",
            desc->NumStaticSamplers, expected_desc->NumStaticSamplers);
    if (!expected_desc->pStaticSamplers)
    {
        ok_(line)(!desc->pStaticSamplers, "Got unexpected static samplers %p.\n", desc->pStaticSamplers);
    }
    else if (desc->NumStaticSamplers == expected_desc->NumStaticSamplers)
    {
        for (i = 0; i < desc->NumStaticSamplers; ++i)
            check_static_sampler_(line, &desc->pStaticSamplers[i], &expected_desc->pStaticSamplers[i]);
    }
    ok_(line)(desc->Flags == expected_desc->Flags, "Got flags %#x, expected %#x.\n",
            desc->Flags, expected_desc->Flags);
}

#define test_root_signature_deserialization(a, b, c) test_root_signature_deserialization_(__LINE__, a, b, c)
static void test_root_signature_deserialization_(unsigned int line, const DWORD *code, size_t code_size,
        const D3D12_ROOT_SIGNATURE_DESC *expected_desc)
{
    ID3D12RootSignatureDeserializer *deserializer;
    const D3D12_ROOT_SIGNATURE_DESC *desc;
    ULONG refcount;
    HRESULT hr;

    hr = D3D12CreateRootSignatureDeserializer(code, code_size,
            &IID_ID3D12RootSignatureDeserializer, (void **)&deserializer);
    ok_(line)(hr == S_OK, "Failed to create deserializer, hr %#x.\n", hr);

    desc = ID3D12RootSignatureDeserializer_GetRootSignatureDesc(deserializer);
    ok(desc, "Got NULL root signature desc.\n");
    check_root_signature_desc_(line, desc, expected_desc);

    refcount = ID3D12RootSignatureDeserializer_Release(deserializer);
    ok_(line)(!refcount, "ID3D12RootSignatureDeserializer has %u references left.\n", (unsigned int)refcount);
}

#define test_root_signature_serialization(a, b, c) test_root_signature_serialization_(__LINE__, a, b, c)
static void test_root_signature_serialization_(unsigned int line, const DWORD *code, size_t code_size,
        const D3D12_ROOT_SIGNATURE_DESC *desc)
{
    DWORD *blob_buffer;
    size_t blob_size;
    ID3DBlob *blob;
    unsigned int i;
    HRESULT hr;

    hr = D3D12SerializeRootSignature(desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, NULL);
    ok_(line)(hr == S_OK, "Failed to serialize root signature, hr %#x.\n", hr);

    blob_buffer = ID3D10Blob_GetBufferPointer(blob);
    blob_size = ID3D10Blob_GetBufferSize(blob);
    ok_(line)(blob_size == code_size, "Got size %u, expected %u.\n",
            (unsigned int)blob_size, (unsigned int)code_size);

    ok_(line)(blob_buffer[0] == code[0], "Got magic %#x, expected %#x.\n",
            (unsigned int)blob_buffer[0], (unsigned int)code[0]);
    for (i = 1; i < 5; ++i)
    {
        todo_(line)(blob_buffer[i] == code[i], "Got checksum %#x, expected %#x at %u.\n",
                (unsigned int)blob_buffer[i], (unsigned int)code[i], i - 1);
    }
    for (; i < code_size / sizeof(DWORD); ++i)
    {
        ok_(line)(blob_buffer[i] == code[i], "Got dword %#x, expected %#x at %u.\n",
                (unsigned int)blob_buffer[i], (unsigned int)code[i], i);
    }

    ID3D10Blob_Release(blob);
}

static void test_root_signature_byte_code(void)
{
    ID3D12RootSignatureDeserializer *deserializer;
    ULONG refcount;
    HRESULT hr;

    /* /T rootsig_1_0 /E RS */
    static const DWORD empty_rootsig[] =
    {
#if 0
        #define RS ""
#endif
        0x43425844, 0xd64afc1d, 0x5dc27735, 0x9edacb4a, 0x6bd8a7fa, 0x00000001, 0x00000044, 0x00000001,
        0x00000024, 0x30535452, 0x00000018, 0x00000001, 0x00000000, 0x00000018, 0x00000000, 0x00000018,
        0x00000000,
    };
    static const D3D12_ROOT_SIGNATURE_DESC empty_rootsig_desc =
    {
        .Flags = 0,
    };
    static const DWORD ia_rootsig[] =
    {
#if 0
        #define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)"
#endif
        0x43425844, 0x05bbd62e, 0xc74d3646, 0xde1407a5, 0x0d99273d, 0x00000001, 0x00000044, 0x00000001,
        0x00000024, 0x30535452, 0x00000018, 0x00000001, 0x00000000, 0x00000018, 0x00000000, 0x00000018,
        0x00000001,
    };
    static const D3D12_ROOT_SIGNATURE_DESC ia_rootsig_desc =
    {
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
    };
    static const DWORD deny_ps_rootsig[] =
    {
#if 0
        #define RS "RootFlags(DENY_PIXEL_SHADER_ROOT_ACCESS)"
#endif
        0x43425844, 0xfad3a4ce, 0xf246286e, 0xeaa9e176, 0x278d5137, 0x00000001, 0x00000044, 0x00000001,
        0x00000024, 0x30535452, 0x00000018, 0x00000001, 0x00000000, 0x00000018, 0x00000000, 0x00000018,
        0x00000020,
    };
    static const D3D12_ROOT_SIGNATURE_DESC deny_ps_rootsig_desc =
    {
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS,
    };
    static const DWORD cbv_rootsig[] =
    {
#if 0
        #define RS "CBV(b3, space = 0)"
#endif
        0x43425844, 0x8dc5087e, 0x5cb9bf0d, 0x2e465ae3, 0x6291e0e0, 0x00000001, 0x00000058, 0x00000001,
        0x00000024, 0x30535452, 0x0000002c, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x0000002c,
        0x00000000, 0x00000002, 0x00000000, 0x00000024, 0x00000003, 0x00000000,

    };
    static const D3D12_ROOT_PARAMETER cbv_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_CBV, .Descriptor = {3, 0}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC cbv_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(cbv_parameters),
        .pParameters = cbv_parameters,
    };
    static const DWORD cbv2_rootsig[] =
    {
#if 0
        #define RS "CBV(b4, space = 1, visibility = SHADER_VISIBILITY_GEOMETRY)"
#endif
        0x43425844, 0x6d4cfb48, 0xbfecaa8d, 0x379ff9c3, 0x0cc56997, 0x00000001, 0x00000058, 0x00000001,
        0x00000024, 0x30535452, 0x0000002c, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x0000002c,
        0x00000000, 0x00000002, 0x00000004, 0x00000024, 0x00000004, 0x00000001,
    };
    static const D3D12_ROOT_PARAMETER cbv2_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_CBV, .Descriptor = {4, 1}, D3D12_SHADER_VISIBILITY_GEOMETRY},
    };
    static const D3D12_ROOT_SIGNATURE_DESC cbv2_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(cbv2_parameters),
        .pParameters = cbv2_parameters,
    };
    static const DWORD srv_rootsig[] =
    {
#if 0
        #define RS "RootFlags(DENY_VERTEX_SHADER_ROOT_ACCESS), SRV(t13)"
#endif
        0x43425844, 0xbc00e5e0, 0xffff2fd3, 0x85c2d405, 0xa61db5e5, 0x00000001, 0x00000058, 0x00000001,
        0x00000024, 0x30535452, 0x0000002c, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x0000002c,
        0x00000002, 0x00000003, 0x00000000, 0x00000024, 0x0000000d, 0x00000000,
    };
    static const D3D12_ROOT_PARAMETER srv_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_SRV, .Descriptor = {13}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC srv_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(srv_parameters),
        .pParameters = srv_parameters,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS,
    };
    static const DWORD uav_rootsig[] =
    {
#if 0
        #define RS "UAV(u6)"
#endif
        0x43425844, 0xf873c52c, 0x69f5cbea, 0xaf6bc9f4, 0x2ccf8b54, 0x00000001, 0x00000058, 0x00000001,
        0x00000024, 0x30535452, 0x0000002c, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x0000002c,
        0x00000000, 0x00000004, 0x00000000, 0x00000024, 0x00000006, 0x00000000,
    };
    static const D3D12_ROOT_PARAMETER uav_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_UAV, .Descriptor = {6}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC uav_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(uav_parameters),
        .pParameters = uav_parameters,
    };
    static const DWORD constants_rootsig[] =
    {
#if 0
        #define RS "RootConstants(num32BitConstants=3, b4), " \
                "RootConstants(num32BitConstants=4, b5, space = 3)"
#endif
        0x43425844, 0xbc015590, 0xa9a4a345, 0x7e446850, 0x2be05281, 0x00000001, 0x00000074, 0x00000001,
        0x00000024, 0x30535452, 0x00000048, 0x00000001, 0x00000002, 0x00000018, 0x00000000, 0x00000048,
        0x00000000, 0x00000001, 0x00000000, 0x00000030, 0x00000001, 0x00000000, 0x0000003c, 0x00000004,
        0x00000000, 0x00000003, 0x00000005, 0x00000003, 0x00000004,
    };
    static const D3D12_ROOT_PARAMETER constants_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = {4, 0, 3}},
        {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = {5, 3, 4}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC constants_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(constants_parameters),
        .pParameters = constants_parameters,
    };
    static const DWORD descriptor_table_rootsig[] =
    {
#if 0
        #define RS "DescriptorTable(CBV(b1, space = 7), " \
                "SRV(t16, numDescriptors = 8), " \
                "UAV(u3, numDescriptors = unbounded, offset = 44))"
#endif
        0x43425844, 0x0f92e563, 0x4766993f, 0x2304e283, 0x14f0d8dc, 0x00000001, 0x00000094, 0x00000001,
        0x00000024, 0x30535452, 0x00000068, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x00000068,
        0x00000000, 0x00000000, 0x00000000, 0x00000024, 0x00000003, 0x0000002c, 0x00000002, 0x00000001,
        0x00000001, 0x00000007, 0xffffffff, 0x00000000, 0x00000008, 0x00000010, 0x00000000, 0xffffffff,
        0x00000001, 0xffffffff, 0x00000003, 0x00000000, 0x0000002c,
    };
    static const D3D12_DESCRIPTOR_RANGE descriptor_ranges[] =
    {
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV,        1,  1, 7, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,        8, 16, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX,  3, 0,                                   44},
    };
    static const D3D12_ROOT_PARAMETER descriptor_table_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                .DescriptorTable = {ARRAY_SIZE(descriptor_ranges), descriptor_ranges}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC descriptor_table_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(descriptor_table_parameters),
        .pParameters = descriptor_table_parameters,
    };
    static const DWORD default_static_sampler_rootsig[] =
    {
#if 0
        #define RS "StaticSampler(s4)"
#endif
        0x43425844, 0x2876b8ff, 0x935aaa0d, 0x5d2d344a, 0xe002147c, 0x00000001, 0x00000078, 0x00000001,
        0x00000024, 0x30535452, 0x0000004c, 0x00000001, 0x00000000, 0x00000018, 0x00000001, 0x00000018,
        0x00000000, 0x00000055, 0x00000001, 0x00000001, 0x00000001, 0x00000000, 0x00000010, 0x00000004,
        0x00000002, 0x00000000, 0x7f7fffff, 0x00000004, 0x00000000, 0x00000000,
    };
    static const D3D12_STATIC_SAMPLER_DESC default_static_sampler_desc =
    {
        .Filter = D3D12_FILTER_ANISOTROPIC,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .MaxAnisotropy = 16,
        .ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
        .BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        .MaxLOD = D3D12_FLOAT32_MAX,
        .ShaderRegister = 4,
    };
    static const D3D12_ROOT_SIGNATURE_DESC default_static_sampler_rootsig_desc =
    {
        .NumStaticSamplers = 1,
        .pStaticSamplers = &default_static_sampler_desc,
    };
    static const DWORD static_samplers_rootsig[] =
    {
#if 0
        #define RS "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_POINT, " \
                "addressV = TEXTURE_ADDRESS_CLAMP, visibility = SHADER_VISIBILITY_PIXEL), " \
                "StaticSampler(s0, filter = FILTER_MIN_MAG_POINT_MIP_LINEAR, " \
                "AddressW = TEXTURE_ADDRESS_BORDER, MipLODBias = 1, maxLod = 10, " \
                "borderColor = STATIC_BORDER_COLOR_OPAQUE_BLACK, space = 3)"
#endif
        0x43425844, 0x52ed526c, 0x892c2d7c, 0xb8ab1123, 0x7e3a727d, 0x00000001, 0x000000ac, 0x00000001,
        0x00000024, 0x30535452, 0x00000080, 0x00000001, 0x00000000, 0x00000018, 0x00000002, 0x00000018,
        0x00000000, 0x00000000, 0x00000001, 0x00000003, 0x00000001, 0x00000000, 0x00000010, 0x00000004,
        0x00000002, 0x00000000, 0x7f7fffff, 0x00000000, 0x00000000, 0x00000005, 0x00000001, 0x00000001,
        0x00000001, 0x00000004, 0x3f800000, 0x00000010, 0x00000004, 0x00000001, 0x00000000, 0x41200000,
        0x00000000, 0x00000003, 0x00000000,
    };
    static const D3D12_STATIC_SAMPLER_DESC static_sampler_descs[] =
    {
        {
            .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .MaxAnisotropy = 16,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
            .BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
            .MaxLOD = D3D12_FLOAT32_MAX,
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
        },
        {
            .Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            .MipLODBias = 1.0f,
            .MaxAnisotropy = 16,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
            .BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
            .MaxLOD = 10.0f,
            .RegisterSpace = 3,
        }
    };
    static const D3D12_ROOT_SIGNATURE_DESC static_samplers_rootsig_desc =
    {
        .NumStaticSamplers = ARRAY_SIZE(static_sampler_descs),
        .pStaticSamplers = static_sampler_descs,
    };

    hr = D3D12CreateRootSignatureDeserializer(empty_rootsig, sizeof(empty_rootsig),
            &IID_IUnknown, (void **)&deserializer);
    ok(hr == E_NOINTERFACE, "Got unexpected hr %#x.\n", hr);

    hr = D3D12CreateRootSignatureDeserializer(empty_rootsig, sizeof(empty_rootsig),
            &IID_ID3D12RootSignatureDeserializer, (void **)&deserializer);
    ok(hr == S_OK, "Failed to create deserializer, hr %#x.\n", hr);

    check_interface(deserializer, &IID_IUnknown, FALSE);
    check_interface(deserializer, &IID_ID3D12RootSignatureDeserializer, TRUE);
    check_interface(deserializer, &IID_ID3D12Object, FALSE);
    check_interface(deserializer, &IID_ID3D12DeviceChild, FALSE);
    check_interface(deserializer, &IID_ID3D12Pageable, FALSE);

    refcount = ID3D12RootSignatureDeserializer_Release(deserializer);
    ok(!refcount, "ID3D12RootSignatureDeserializer has %u references left.\n", (unsigned int)refcount);

    test_root_signature_deserialization(empty_rootsig, sizeof(empty_rootsig), &empty_rootsig_desc);
    test_root_signature_deserialization(ia_rootsig, sizeof(ia_rootsig), &ia_rootsig_desc);
    test_root_signature_deserialization(deny_ps_rootsig, sizeof(deny_ps_rootsig), &deny_ps_rootsig_desc);
    test_root_signature_deserialization(cbv_rootsig, sizeof(cbv_rootsig), &cbv_rootsig_desc);
    test_root_signature_deserialization(cbv2_rootsig, sizeof(cbv2_rootsig), &cbv2_rootsig_desc);
    test_root_signature_deserialization(srv_rootsig, sizeof(srv_rootsig), &srv_rootsig_desc);
    test_root_signature_deserialization(uav_rootsig, sizeof(uav_rootsig), &uav_rootsig_desc);
    test_root_signature_deserialization(constants_rootsig, sizeof(constants_rootsig), &constants_rootsig_desc);
    test_root_signature_deserialization(descriptor_table_rootsig, sizeof(descriptor_table_rootsig),
            &descriptor_table_rootsig_desc);
    test_root_signature_deserialization(default_static_sampler_rootsig, sizeof(default_static_sampler_rootsig),
            &default_static_sampler_rootsig_desc);
    test_root_signature_deserialization(static_samplers_rootsig, sizeof(static_samplers_rootsig),
            &static_samplers_rootsig_desc);

    test_root_signature_serialization(empty_rootsig, sizeof(empty_rootsig), &empty_rootsig_desc);
    test_root_signature_serialization(ia_rootsig, sizeof(ia_rootsig), &ia_rootsig_desc);
    test_root_signature_serialization(deny_ps_rootsig, sizeof(deny_ps_rootsig), &deny_ps_rootsig_desc);
    test_root_signature_serialization(cbv_rootsig, sizeof(cbv_rootsig), &cbv_rootsig_desc);
    test_root_signature_serialization(cbv2_rootsig, sizeof(cbv2_rootsig), &cbv2_rootsig_desc);
    test_root_signature_serialization(srv_rootsig, sizeof(srv_rootsig), &srv_rootsig_desc);
    test_root_signature_serialization(uav_rootsig, sizeof(uav_rootsig), &uav_rootsig_desc);
    test_root_signature_serialization(constants_rootsig, sizeof(constants_rootsig), &constants_rootsig_desc);
    test_root_signature_serialization(descriptor_table_rootsig, sizeof(descriptor_table_rootsig),
            &descriptor_table_rootsig_desc);
    test_root_signature_serialization(default_static_sampler_rootsig, sizeof(default_static_sampler_rootsig),
            &default_static_sampler_rootsig_desc);
    test_root_signature_serialization(static_samplers_rootsig, sizeof(static_samplers_rootsig),
            &static_samplers_rootsig_desc);
}

static void test_cs_constant_buffer(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor_handle;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[2];
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;
    ID3D12Resource *resource, *cb;
    unsigned int descriptor_size;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    unsigned int i;
    float value;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        cbuffer cb : register(b7)
        {
            float value;
        };

        RWBuffer<float> buffer;

        [numthreads(32, 1, 1)]
        void main(uint3 group_id : SV_groupID, uint group_index : SV_GroupIndex)
        {
            uint global_index = 32 * group_id.x + group_index;
            buffer[global_index] = value;
        }
#endif
        0x43425844, 0xbcbca6fb, 0x0bd883e5, 0x8e0848ea, 0xaf152cfd, 0x00000001, 0x000000e8, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000094, 0x00050050, 0x00000025, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000007, 0x00000001, 0x0400089c, 0x0011e000, 0x00000000, 0x00005555,
        0x0200005f, 0x00024000, 0x0200005f, 0x00021012, 0x02000068, 0x00000001, 0x0400009b, 0x00000020,
        0x00000001, 0x00000001, 0x07000023, 0x00100012, 0x00000000, 0x0002100a, 0x00004001, 0x00000020,
        0x0002400a, 0x080000a4, 0x0011e0f2, 0x00000000, 0x00100006, 0x00000000, 0x00208006, 0x00000007,
        0x00000000, 0x0100003e,
    };

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    value = 2.0f;
    cb = create_upload_buffer(context.device, sizeof(value), &value);

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 4;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[1].Descriptor.ShaderRegister = 7;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    pipeline_state = create_compute_pipeline_state(device, root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 4;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap);
    gpu_descriptor_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap);

    resource = create_default_buffer(device, 64 * sizeof(float),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = 64;
    uav_desc.Buffer.StructureByteStride = 0;
    uav_desc.Buffer.CounterOffsetInBytes = 0;
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    ID3D12Device_CreateUnorderedAccessView(device, resource, NULL, &uav_desc, cpu_descriptor_handle);
    /* For tier 1 hardware all descriptors must be populated. */
    for (i = 1; i < heap_desc.NumDescriptors; ++i)
    {
        cpu_descriptor_handle.ptr += descriptor_size;
        ID3D12Device_CreateUnorderedAccessView(device, NULL, NULL, &uav_desc, cpu_descriptor_handle);
    }

    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_Dispatch(command_list, 2, 1, 1);

    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(resource, uav_desc.Format, &rb, queue, command_list);
    check_readback_data_float(&rb, NULL, 2.0f, 0);
    release_resource_readback(&rb);

    value = 6.0f;
    update_buffer_data(cb, 0, sizeof(value), &value);

    reset_command_list(command_list, context.allocator);
    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_Dispatch(command_list, 2, 1, 1);

    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(resource, uav_desc.Format, &rb, queue, command_list);
    check_readback_data_float(&rb, NULL, 6.0f, 0);
    release_resource_readback(&rb);

    ID3D12Resource_Release(cb);
    ID3D12Resource_Release(resource);
    ID3D12RootSignature_Release(root_signature);
    ID3D12PipelineState_Release(pipeline_state);
    ID3D12DescriptorHeap_Release(descriptor_heap);
    destroy_test_context(&context);
}

static void test_constant_buffer_relative_addressing(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_ROOT_PARAMETER root_parameters[2];
    ID3D12GraphicsCommandList *command_list;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Resource *uav, *cb;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        cbuffer b0
        {
            uint4 pad;
            uint4 data[4];
        };

        RWByteAddressBuffer u0;

        [numthreads(4, 1, 1)]
        void main(uint tid : SV_GroupThreadID)
        {
            uint location = 4 * tid;
            u0.Store4(4 * location, data[tid]);
        }
#endif
        0x43425844, 0x759a28a0, 0xdd34cd41, 0x73702692, 0x739a66ea, 0x00000001, 0x000000f0, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x0000009c, 0x00050050, 0x00000027, 0x0100086a,
        0x04000859, 0x00208e46, 0x00000000, 0x00000005, 0x0300009d, 0x0011e000, 0x00000000, 0x0200005f,
        0x00022012, 0x02000068, 0x00000001, 0x0400009b, 0x00000004, 0x00000001, 0x00000001, 0x06000029,
        0x00100012, 0x00000000, 0x0002200a, 0x00004001, 0x00000004, 0x04000036, 0x00100022, 0x00000000,
        0x0002200a, 0x0a0000a6, 0x0011e0f2, 0x00000000, 0x0010000a, 0x00000000, 0x06208e46, 0x00000000,
        0x00000001, 0x0010001a, 0x00000000, 0x0100003e,
    };
    static const struct uvec4 cb_data[] =
    {
        {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef},
        {1, 2, 3, 4},
        {4, 4, 9, 8},
        {4, 5, 6, 7},
        {6, 0, 6, 0},
    };

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    cb = create_upload_buffer(context.device, sizeof(cb_data), &cb_data);

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[0].Descriptor.RegisterSpace = 0;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    uav = create_default_buffer(device, 16 * sizeof(uint32_t),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(command_list, 0,
            ID3D12Resource_GetGPUVirtualAddress(uav));
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    transition_sub_resource_state(command_list, uav, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(uav, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < rb.width; ++i)
    {
        unsigned int got = get_readback_uint(&rb, i, 0);
        const unsigned int *expected = &cb_data[1].x;
        ok(got == expected[i], "Got %#x, expected %#x at %u.\n", got, expected[i], i);
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(cb);
    ID3D12Resource_Release(uav);
    destroy_test_context(&context);
}

static void test_immediate_constant_buffer(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    struct test_context_desc desc;
    struct test_context context;
    unsigned int index[4] = {};
    ID3D12CommandQueue *queue;
    ID3D12Resource *cb;
    unsigned int i;

    static const DWORD ps_code[] =
    {
#if 0
        uint index;

        static const int int_array[6] =
        {
            310, 111, 212, -513, -318, 0,
        };

        static const uint uint_array[6] =
        {
            2, 7, 0x7f800000, 0xff800000, 0x7fc00000, 0
        };

        static const float float_array[6] =
        {
            76, 83.5f, 0.5f, 0.75f, -0.5f, 0.0f,
        };

        float4 main() : SV_Target
        {
            return float4(int_array[index], uint_array[index], float_array[index], 1.0f);
        }
#endif
        0x43425844, 0xbad068da, 0xd631ea3c, 0x41648374, 0x3ccd0120, 0x00000001, 0x00000184, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x0000010c, 0x00000040, 0x00000043,
        0x00001835, 0x0000001a, 0x00000136, 0x00000002, 0x42980000, 0x00000000, 0x0000006f, 0x00000007,
        0x42a70000, 0x00000000, 0x000000d4, 0x7f800000, 0x3f000000, 0x00000000, 0xfffffdff, 0xff800000,
        0x3f400000, 0x00000000, 0xfffffec2, 0x7fc00000, 0xbf000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2,
        0x00000000, 0x02000068, 0x00000001, 0x05000036, 0x00102082, 0x00000000, 0x00004001, 0x3f800000,
        0x06000036, 0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x06000056, 0x00102022,
        0x00000000, 0x0090901a, 0x0010000a, 0x00000000, 0x0600002b, 0x00102012, 0x00000000, 0x0090900a,
        0x0010000a, 0x00000000, 0x06000036, 0x00102042, 0x00000000, 0x0090902a, 0x0010000a, 0x00000000,
        0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static struct vec4 expected_result[] =
    {
        { 310.0f,          2.0f, 76.00f, 1.0f},
        { 111.0f,          7.0f, 83.50f, 1.0f},
        { 212.0f, 2139095040.0f,  0.50f, 1.0f},
        {-513.0f, 4286578688.0f,  0.75f, 1.0f},
        {-318.0f, 2143289344.0f, -0.50f, 1.0f},
        {   0.0f,          0.0f,  0.0f,  1.0f},
    };

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_cb_root_signature(context.device,
            0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, NULL, &ps, NULL);

    cb = create_upload_buffer(context.device, sizeof(index), NULL);

    for (i = 0; i < ARRAY_SIZE(expected_result); ++i)
    {
        *index = i;
        update_buffer_data(cb, 0, sizeof(index), index);

        if (i)
            transition_resource_state(command_list, context.render_target,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list, 0,
                ID3D12Resource_GetGPUVirtualAddress(cb));
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        transition_resource_state(command_list, context.render_target,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check_sub_resource_vec4(context.render_target, 0, queue, command_list, &expected_result[i], 0);

        reset_command_list(command_list, context.allocator);
    }

    ID3D12Resource_Release(cb);
    destroy_test_context(&context);
}

static void test_root_constants(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const unsigned int constants[4] = {0, 1, 0, 2};

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_ROOT_PARAMETER root_parameters[3];
    ID3D12GraphicsCommandList *command_list;
    struct vec4 vs_cb_color, ps_cb_color;
    struct test_context_desc desc;
    struct test_context context;
    struct vec4 expected_result;
    ID3D12CommandQueue *queue;
    HRESULT hr;

    static const DWORD ps_uint_constant_code[] =
    {
#if 0
        uint4 constants;

        float4 main() : SV_Target
        {
            return (float4)constants;
        }
#endif
        0x43425844, 0xf744186d, 0x6805439a, 0x491c3625, 0xe3e4053c, 0x00000001, 0x000000bc, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000044, 0x00000050, 0x00000011,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x06000056, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_uint_constant = {ps_uint_constant_code, sizeof(ps_uint_constant_code)};
    static const DWORD vs_color_code[] =
    {
#if 0
        float4 constant_color;

        void main(uint id : SV_VertexID,
                out float4 position : SV_Position, out float4 color : COLOR)
        {
            float2 coords = float2((id << 1) & 2, id & 2);
            position = float4(coords * float2(2, -2) + float2(-1, 1), 0, 1);
            color = constant_color;
        }
#endif
        0x43425844, 0x7c3173fb, 0xdd990625, 0x290ad676, 0x50b41793, 0x00000001, 0x000001e0, 0x00000003,
        0x0000002c, 0x00000060, 0x000000b4, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000006, 0x00000001, 0x00000000, 0x00000101, 0x565f5653, 0x65747265, 0x00444978,
        0x4e47534f, 0x0000004c, 0x00000002, 0x00000008, 0x00000038, 0x00000000, 0x00000001, 0x00000003,
        0x00000000, 0x0000000f, 0x00000044, 0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x0000000f,
        0x505f5653, 0x7469736f, 0x006e6f69, 0x4f4c4f43, 0xabab0052, 0x58454853, 0x00000124, 0x00010050,
        0x00000049, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x04000060, 0x00101012,
        0x00000000, 0x00000006, 0x04000067, 0x001020f2, 0x00000000, 0x00000001, 0x03000065, 0x001020f2,
        0x00000001, 0x02000068, 0x00000001, 0x0b00008c, 0x00100012, 0x00000000, 0x00004001, 0x00000001,
        0x00004001, 0x00000001, 0x0010100a, 0x00000000, 0x00004001, 0x00000000, 0x07000001, 0x00100042,
        0x00000000, 0x0010100a, 0x00000000, 0x00004001, 0x00000002, 0x05000056, 0x00100032, 0x00000000,
        0x00100086, 0x00000000, 0x0f000032, 0x00102032, 0x00000000, 0x00100046, 0x00000000, 0x00004002,
        0x40000000, 0xc0000000, 0x00000000, 0x00000000, 0x00004002, 0xbf800000, 0x3f800000, 0x00000000,
        0x00000000, 0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x3f800000, 0x06000036, 0x001020f2, 0x00000001, 0x00208e46, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs_color = {vs_color_code, sizeof(vs_color_code)};
    static const DWORD ps_color_code[] =
    {
#if 0
        float4 color;

        float4 main(float4 position : SV_POSITION, float4 in_color : COLOR) : SV_Target
        {
            if (any(color != in_color))
                return float4(0.0f, 0.0f, 1.0f, 1.0f);
            return in_color;
        }
#endif
        0x43425844, 0xb1e305a3, 0x962c4d64, 0x6b2c5515, 0x4fb4f524, 0x00000001, 0x0000019c, 0x00000003,
        0x0000002c, 0x00000080, 0x000000b4, 0x4e475349, 0x0000004c, 0x00000002, 0x00000008, 0x00000038,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x00000044, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000f0f, 0x505f5653, 0x5449534f, 0x004e4f49, 0x4f4c4f43, 0xabab0052,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000e0, 0x00000050,
        0x00000038, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03001062, 0x001010f2,
        0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x08000039, 0x001000f2,
        0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00101e46, 0x00000001, 0x0700003c, 0x00100032,
        0x00000000, 0x00100ae6, 0x00000000, 0x00100046, 0x00000000, 0x0700003c, 0x00100012, 0x00000000,
        0x0010001a, 0x00000000, 0x0010000a, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x3f800000, 0x3f800000, 0x0100003e,
        0x01000015, 0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_color = {ps_color_code, sizeof(ps_color_code)};
    static const DWORD vs_mix_code[] =
    {
#if 0
        cbuffer shared_cb
        {
            uint token;
            uint op;
        };

        cbuffer vs_cb
        {
            float4 padding;
            float4 vs_color;
        };

        void main(uint id : SV_VertexID,
                out float4 position : SV_Position, out float4 color : COLOR,
                out uint vs_token : TOKEN)
        {
            float2 coords = float2((id << 1) & 2, id & 2);
            position = float4(coords * float2(2, -2) + float2(-1, 1), 0, 1);
            color = vs_color;
            vs_token = token;
        }
#endif
        0x43425844, 0xb5bc00c3, 0x6b5041fe, 0xd55d1d86, 0x34a2a229, 0x00000001, 0x00000230, 0x00000003,
        0x0000002c, 0x00000060, 0x000000d0, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000006, 0x00000001, 0x00000000, 0x00000101, 0x565f5653, 0x65747265, 0x00444978,
        0x4e47534f, 0x00000068, 0x00000003, 0x00000008, 0x00000050, 0x00000000, 0x00000001, 0x00000003,
        0x00000000, 0x0000000f, 0x0000005c, 0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x0000000f,
        0x00000062, 0x00000000, 0x00000000, 0x00000001, 0x00000002, 0x00000e01, 0x505f5653, 0x7469736f,
        0x006e6f69, 0x4f4c4f43, 0x4f540052, 0x004e454b, 0x58454853, 0x00000158, 0x00010050, 0x00000056,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x04000059, 0x00208e46, 0x00000001,
        0x00000002, 0x04000060, 0x00101012, 0x00000000, 0x00000006, 0x04000067, 0x001020f2, 0x00000000,
        0x00000001, 0x03000065, 0x001020f2, 0x00000001, 0x03000065, 0x00102012, 0x00000002, 0x02000068,
        0x00000001, 0x0b00008c, 0x00100012, 0x00000000, 0x00004001, 0x00000001, 0x00004001, 0x00000001,
        0x0010100a, 0x00000000, 0x00004001, 0x00000000, 0x07000001, 0x00100042, 0x00000000, 0x0010100a,
        0x00000000, 0x00004001, 0x00000002, 0x05000056, 0x00100032, 0x00000000, 0x00100086, 0x00000000,
        0x0f000032, 0x00102032, 0x00000000, 0x00100046, 0x00000000, 0x00004002, 0x40000000, 0xc0000000,
        0x00000000, 0x00000000, 0x00004002, 0xbf800000, 0x3f800000, 0x00000000, 0x00000000, 0x08000036,
        0x001020c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x3f800000, 0x06000036,
        0x001020f2, 0x00000001, 0x00208e46, 0x00000001, 0x00000001, 0x06000036, 0x00102012, 0x00000002,
        0x0020800a, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs_mix = {vs_mix_code, sizeof(vs_mix_code)};
    static const DWORD ps_mix_code[] =
    {
#if 0
        cbuffer shared_cb
        {
            uint token;
            uint op;
        };

        cbuffer ps_cb
        {
            float4 ps_color;
        };

        float4 main(float4 position : SV_POSITION, float4 vs_color : COLOR,
                uint vs_token : TOKEN) : SV_Target
        {
            if (token != vs_token)
                return (float4)1.0f;

            switch (op)
            {
                case 0: return vs_color;
                case 1: return ps_color;
                case 2: return vs_color * ps_color;
                default: return (float4)0.0f;
            }
        }
#endif
        0x43425844, 0x128ef4ce, 0xa1c46517, 0x34ca76f3, 0x3c7d6112, 0x00000001, 0x00000240, 0x00000003,
        0x0000002c, 0x0000009c, 0x000000d0, 0x4e475349, 0x00000068, 0x00000003, 0x00000008, 0x00000050,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000f0f, 0x00000062, 0x00000000, 0x00000000, 0x00000001, 0x00000002,
        0x00000101, 0x505f5653, 0x5449534f, 0x004e4f49, 0x4f4c4f43, 0x4f540052, 0x004e454b, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000168, 0x00000050, 0x0000005a,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x04000059, 0x00208e46, 0x00000001,
        0x00000001, 0x03001062, 0x001010f2, 0x00000001, 0x03000862, 0x00101012, 0x00000002, 0x03000065,
        0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x08000027, 0x00100012, 0x00000000, 0x0020800a,
        0x00000000, 0x00000000, 0x0010100a, 0x00000002, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x0100003e,
        0x01000015, 0x0400004c, 0x0020801a, 0x00000000, 0x00000000, 0x03000006, 0x00004001, 0x00000000,
        0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000001, 0x0100003e, 0x03000006, 0x00004001,
        0x00000001, 0x06000036, 0x001020f2, 0x00000000, 0x00208e46, 0x00000001, 0x00000000, 0x0100003e,
        0x03000006, 0x00004001, 0x00000002, 0x08000038, 0x001020f2, 0x00000000, 0x00101e46, 0x00000001,
        0x00208e46, 0x00000001, 0x00000000, 0x0100003e, 0x0100000a, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e, 0x01000017, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_mix = {ps_mix_code, sizeof(ps_mix_code)};

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_32bit_constants_root_signature(context.device,
            0, ARRAY_SIZE(constants), D3D12_SHADER_VISIBILITY_ALL);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, NULL, &ps_uint_constant, NULL);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0,
            ARRAY_SIZE(constants), constants, 0);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    expected_result.x = constants[0];
    expected_result.y = constants[1];
    expected_result.z = constants[2];
    expected_result.w = constants[3];
    check_sub_resource_vec4(context.render_target, 0, queue, command_list, &expected_result, 0);

    reset_command_list(command_list, context.allocator);

    ID3D12PipelineState_Release(context.pipeline_state);
    ID3D12RootSignature_Release(context.root_signature);

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].Constants.ShaderRegister = 0;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = 4;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 0;
    root_parameters[1].Constants.RegisterSpace = 0;
    root_parameters[1].Constants.Num32BitValues = 4;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(context.device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, &vs_color, &ps_color, NULL);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    vs_cb_color = ps_cb_color = expected_result = (struct vec4){0.0f, 1.0f, 0.0f, 1.0f};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 4, &vs_cb_color.x, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &ps_cb_color.x, 0);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_vec4(context.render_target, 0, queue, command_list, &expected_result, 0);

    reset_command_list(command_list, context.allocator);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    vs_cb_color = (struct vec4){0.0f, 1.0f, 0.0f, 1.0f};
    ps_cb_color = (struct vec4){1.0f, 1.0f, 1.0f, 1.0f};
    expected_result = (struct vec4){0.0f, 0.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 4, &vs_cb_color.x, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &ps_cb_color.x, 0);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_vec4(context.render_target, 0, queue, command_list, &expected_result, 0);

    reset_command_list(command_list, context.allocator);

    ID3D12PipelineState_Release(context.pipeline_state);
    ID3D12RootSignature_Release(context.root_signature);

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].Constants.ShaderRegister = 1;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = 8;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 1;
    root_parameters[1].Constants.RegisterSpace = 0;
    root_parameters[1].Constants.Num32BitValues = 4;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[2].Constants.ShaderRegister = 0;
    root_parameters[2].Constants.RegisterSpace = 0;
    root_parameters[2].Constants.Num32BitValues = 2;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 3;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(context.device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, &vs_mix, &ps_mix, NULL);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    vs_cb_color = expected_result = (struct vec4){0.0f, 1.0f, 0.0f, 1.0f};
    ps_cb_color = (struct vec4){1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 4, &vs_cb_color.x, 4);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &ps_cb_color.x, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(command_list, 2, 0xdeadbeef, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(command_list, 2, 0, 1);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_vec4(context.render_target, 0, queue, command_list, &expected_result, 0);

    reset_command_list(command_list, context.allocator);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    vs_cb_color = (struct vec4){0.0f, 1.0f, 0.0f, 1.0f};
    ps_cb_color = expected_result = (struct vec4){1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 4, &vs_cb_color.x, 4);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &ps_cb_color.x, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(command_list, 2, 0xdeadbeef, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(command_list, 2, 1, 1);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_vec4(context.render_target, 0, queue, command_list, &expected_result, 0);

    reset_command_list(command_list, context.allocator);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    vs_cb_color = (struct vec4){0.5f, 1.0f, 0.5f, 1.0f};
    ps_cb_color = (struct vec4){0.5f, 0.7f, 1.0f, 1.0f};
    expected_result = (struct vec4){0.25f, 0.7f, 0.5f, 1.0f};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 4, &vs_cb_color.x, 4);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &ps_cb_color.x, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(command_list, 2, 0xdeadbeef, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(command_list, 2, 2, 1);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_vec4(context.render_target, 0, queue, command_list, &expected_result, 0);

    destroy_test_context(&context);
}

static void test_texture(void)
{
    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    D3D12_SUBRESOURCE_DATA texture_data;
    struct test_context_desc desc;
    struct resource_readback rb;
    struct test_context context;
    ID3D12DescriptorHeap *heap;
    ID3D12CommandQueue *queue;
    ID3D12Resource *texture;
    unsigned int x, y;
    HRESULT hr;

    static const DWORD ps_code[] =
    {
#if 0
        Texture2D t;
        SamplerState s;

        float4 main(float4 position : SV_POSITION) : SV_Target
        {
            float2 p;

            p.x = position.x / 32.0f;
            p.y = position.y / 32.0f;
            return t.Sample(s, p);
        }
#endif
        0x43425844, 0x7a0c3929, 0x75ff3ca4, 0xccb318b2, 0xe6965b4c, 0x00000001, 0x00000140, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x5449534f, 0x004e4f49,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000a4, 0x00000050,
        0x00000029, 0x0100086a, 0x0300005a, 0x00106000, 0x00000000, 0x04001858, 0x00107000, 0x00000000,
        0x00005555, 0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x0a000038, 0x00100032, 0x00000000, 0x00101046, 0x00000000, 0x00004002,
        0x3d000000, 0x3d000000, 0x00000000, 0x00000000, 0x8b000045, 0x800000c2, 0x00155543, 0x001020f2,
        0x00000000, 0x00100046, 0x00000000, 0x00107e46, 0x00000000, 0x00106000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const float red[] = {1.0f, 0.0f, 0.0f, 0.5f};
    static const unsigned int bitmap_data[] =
    {
        0xff0000ff, 0xff00ffff, 0xff00ff00, 0xffffff00,
        0xffff0000, 0xffff00ff, 0xff000000, 0xff7f7f7f,
        0xffffffff, 0xffffffff, 0xffffffff, 0xff000000,
        0xffffffff, 0xff000000, 0xff000000, 0xff000000,
    };

    memset(&desc, 0, sizeof(desc));
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_texture_root_signature(context.device,
            D3D12_SHADER_VISIBILITY_PIXEL, 0, 0);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps, NULL);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    gpu_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);

    texture = create_texture(context.device, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST);
    texture_data.pData = bitmap_data;
    texture_data.RowPitch = 4 * sizeof(*bitmap_data);
    texture_data.SlicePitch = texture_data.RowPitch * 4;
    upload_texture_data(texture, &texture_data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);

    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ID3D12Device_CreateShaderResourceView(context.device, texture, NULL, cpu_handle);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, red, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gpu_handle);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < 4; ++y)
    {
        for (x = 0; x < 4; ++x)
        {
           unsigned int color = get_readback_uint(&rb, 4 + 8 * x, 4 + 8 * y);
           ok(compare_color(color, bitmap_data[4 * y + x], 1),
                   "Got color 0x%08x, expected 0x%08x at (%u, %u).\n", color, bitmap_data[4 * y + x], x, y);
        }
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(texture);
    ID3D12DescriptorHeap_Release(heap);
    destroy_test_context(&context);
}

static void test_gather(void)
{
    struct
    {
        int width, height;
        int offset_x, offset_y;
    } constants;

    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    struct test_context_desc desc;
    struct resource_readback rb;
    struct test_context context;
    ID3D12DescriptorHeap *heap;
    ID3D12CommandQueue *queue;
    ID3D12Resource *texture;
    unsigned int x, y;
    HRESULT hr;

    static const DWORD gather4_code[] =
    {
#if 0
        SamplerState s;
        Texture2D<float4> t;

        int2 size;

        float4 main(float4 position : SV_Position) : SV_Target
        {
            return t.Gather(s, position.xy / size);
        }
#endif
        0x43425844, 0xca1ee692, 0xb122f477, 0x8c467d38, 0x0f5a233a, 0x00000001, 0x00000154, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x000000b8, 0x00000041,
        0x0000002e, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300005a, 0x00106000,
        0x00000000, 0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x04002064, 0x00101032, 0x00000000,
        0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x0600002b, 0x00100032,
        0x00000000, 0x00208046, 0x00000000, 0x00000000, 0x0700000e, 0x00100032, 0x00000000, 0x00101046,
        0x00000000, 0x00100046, 0x00000000, 0x0900006d, 0x001020f2, 0x00000000, 0x00100046, 0x00000000,
        0x00107e46, 0x00000000, 0x0010600a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_gather4 = {gather4_code, sizeof(gather4_code)};
    static const DWORD gather4_offset_code[] =
    {
#if 0
        SamplerState s;
        Texture2D<float4> t;

        int2 size;

        float4 main(float4 position : SV_Position) : SV_Target
        {
            return t.Gather(s, position.xy / size, int2(1, 1));
        }
#endif
        0x43425844, 0xe5ab2216, 0x90748ece, 0x7ccf2123, 0x4edbba7c, 0x00000001, 0x00000158, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x000000bc, 0x00000041,
        0x0000002f, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300005a, 0x00106000,
        0x00000000, 0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x04002064, 0x00101032, 0x00000000,
        0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x0600002b, 0x00100032,
        0x00000000, 0x00208046, 0x00000000, 0x00000000, 0x0700000e, 0x00100032, 0x00000000, 0x00101046,
        0x00000000, 0x00100046, 0x00000000, 0x8a00006d, 0x00002201, 0x001020f2, 0x00000000, 0x00100046,
        0x00000000, 0x00107e46, 0x00000000, 0x0010600a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_gather4_offset = {gather4_offset_code, sizeof(gather4_offset_code)};
    static const DWORD gather4_green_code[] =
    {
#if 0
        SamplerState s;
        Texture2D<float4> t;

        int2 size;

        float4 main(float4 position : SV_Position) : SV_Target
        {
            return t.GatherGreen(s, position.xy / size);
        }
#endif
        0x43425844, 0x2b0ad2d9, 0x8ad30b52, 0xc418477f, 0xe5211693, 0x00000001, 0x0000015c, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000c0, 0x00000050,
        0x00000030, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300005a, 0x00106000,
        0x00000000, 0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x04002064, 0x00101032, 0x00000000,
        0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x0600002b, 0x00100032,
        0x00000000, 0x00208046, 0x00000000, 0x00000000, 0x0700000e, 0x00100032, 0x00000000, 0x00101046,
        0x00000000, 0x00100046, 0x00000000, 0x8b00006d, 0x800000c2, 0x00155543, 0x001020f2, 0x00000000,
        0x00100046, 0x00000000, 0x00107e46, 0x00000000, 0x0010601a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_gather4_green = {gather4_green_code, sizeof(gather4_green_code)};
    static const DWORD gather4_po_code[] =
    {
#if 0
        SamplerState s;
        Texture2D<float4> t;

        int2 size;
        int2 offset;

        float4 main(float4 position : SV_Position) : SV_Target
        {
            return t.Gather(s, position.xy / size, offset);
        }
#endif
        0x43425844, 0xe19bdd35, 0x44514fb3, 0xfaa8727f, 0xc1092da0, 0x00000001, 0x00000168, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000cc, 0x00000050,
        0x00000033, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300005a, 0x00106000,
        0x00000000, 0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x04002064, 0x00101032, 0x00000000,
        0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x0600002b, 0x00100032,
        0x00000000, 0x00208046, 0x00000000, 0x00000000, 0x0700000e, 0x00100032, 0x00000000, 0x00101046,
        0x00000000, 0x00100046, 0x00000000, 0x8e00007f, 0x800000c2, 0x00155543, 0x001020f2, 0x00000000,
        0x00100046, 0x00000000, 0x00208ae6, 0x00000000, 0x00000000, 0x00107e46, 0x00000000, 0x0010600a,
        0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_gather4_po = {gather4_po_code, sizeof(gather4_po_code)};
    static const struct vec4 texture_data[] =
    {
        {0.0f, 0.0f}, {1.0f, 1.0f}, {2.0f, 2.0f}, {3.0f, 3.0f},
        {4.0f, 0.1f}, {5.0f, 1.1f}, {6.0f, 2.1f}, {7.0f, 3.1f},
        {8.0f, 0.2f}, {9.0f, 1.2f}, {0.5f, 2.2f}, {1.5f, 3.2f},
        {2.5f, 0.3f}, {3.5f, 1.3f}, {4.5f, 2.3f}, {5.5f, 3.3f},
    };
    static const struct vec4 expected_gather4[] =
    {
        {4.0f, 5.0f, 1.0f, 0.0f}, {5.0f, 6.0f, 2.0f, 1.0f}, {6.0f, 7.0f, 3.0f, 2.0f}, {7.0f, 7.0f, 3.0f, 3.0f},
        {8.0f, 9.0f, 5.0f, 4.0f}, {9.0f, 0.5f, 6.0f, 5.0f}, {0.5f, 1.5f, 7.0f, 6.0f}, {1.5f, 1.5f, 7.0f, 7.0f},
        {2.5f, 3.5f, 9.0f, 8.0f}, {3.5f, 4.5f, 0.5f, 9.0f}, {4.5f, 5.5f, 1.5f, 0.5f}, {5.5f, 5.5f, 1.5f, 1.5f},
        {2.5f, 3.5f, 3.5f, 2.5f}, {3.5f, 4.5f, 4.5f, 3.5f}, {4.5f, 5.5f, 5.5f, 4.5f}, {5.5f, 5.5f, 5.5f, 5.5f},
    };
    static const struct vec4 expected_gather4_offset[] =
    {
        {9.0f, 0.5f, 6.0f, 5.0f}, {0.5f, 1.5f, 7.0f, 6.0f}, {1.5f, 1.5f, 7.0f, 7.0f}, {1.5f, 1.5f, 7.0f, 7.0f},
        {3.5f, 4.5f, 0.5f, 9.0f}, {4.5f, 5.5f, 1.5f, 0.5f}, {5.5f, 5.5f, 1.5f, 1.5f}, {5.5f, 5.5f, 1.5f, 1.5f},
        {3.5f, 4.5f, 4.5f, 3.5f}, {4.5f, 5.5f, 5.5f, 4.5f}, {5.5f, 5.5f, 5.5f, 5.5f}, {5.5f, 5.5f, 5.5f, 5.5f},
        {3.5f, 4.5f, 4.5f, 3.5f}, {4.5f, 5.5f, 5.5f, 4.5f}, {5.5f, 5.5f, 5.5f, 5.5f}, {5.5f, 5.5f, 5.5f, 5.5f},
    };
    static const struct vec4 expected_gather4_green[] =
    {
        {0.1f, 1.1f, 1.0f, 0.0f}, {1.1f, 2.1f, 2.0f, 1.0f}, {2.1f, 3.1f, 3.0f, 2.0f}, {3.1f, 3.1f, 3.0f, 3.0f},
        {0.2f, 1.2f, 1.1f, 0.1f}, {1.2f, 2.2f, 2.1f, 1.1f}, {2.2f, 3.2f, 3.1f, 2.1f}, {3.2f, 3.2f, 3.1f, 3.1f},
        {0.3f, 1.3f, 1.2f, 0.2f}, {1.3f, 2.3f, 2.2f, 1.2f}, {2.3f, 3.3f, 3.2f, 2.2f}, {3.3f, 3.3f, 3.2f, 3.2f},
        {0.3f, 1.3f, 1.3f, 0.3f}, {1.3f, 2.3f, 2.3f, 1.3f}, {2.3f, 3.3f, 3.3f, 2.3f}, {3.3f, 3.3f, 3.3f, 3.3f},
    };
    static const struct vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
    static const D3D12_SUBRESOURCE_DATA resource_data = {&texture_data, sizeof(texture_data) / 4};

    memset(&desc, 0, sizeof(desc));
    desc.rt_width = 4;
    desc.rt_height = 4;
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_texture_root_signature(context.device,
            D3D12_SHADER_VISIBILITY_PIXEL, 4, 0);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    gpu_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);

    texture = create_texture(context.device, 4, 4, DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Device_CreateShaderResourceView(context.device, texture, NULL, cpu_handle);
    upload_texture_data(texture, &resource_data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    constants.width = 4;
    constants.height = 4;
    constants.offset_x = 1;
    constants.offset_y = 1;

    /* ps_gather4 */
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, NULL, &ps_gather4, NULL);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, &white.x, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gpu_handle);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &constants.width, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < rb.height; ++y)
    {
        for (x = 0; x < rb.width; ++x)
        {
            const struct vec4 *expected = &expected_gather4[y * rb.width + x];
            const struct vec4 *got = get_readback_vec4(&rb, x, y);
            ok(compare_vec4(got, expected, 0),
                    "Got {%.8e, %.8e, %.8e, %.8e}, expected {%.8e, %.8e, %.8e, %.8e}.\n",
                    got->x, got->y, got->z, got->w, expected->x, expected->y, expected->z, expected->w);
        }
    }
    release_resource_readback(&rb);

    ID3D12PipelineState_Release(context.pipeline_state);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    /* ps_gather4_offset */
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, NULL, &ps_gather4_offset, NULL);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, &white.x, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gpu_handle);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &constants.width, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < rb.height; ++y)
    {
        for (x = 0; x < rb.width; ++x)
        {
            const struct vec4 *expected = &expected_gather4_offset[y * rb.width + x];
            const struct vec4 *got = get_readback_vec4(&rb, x, y);
            ok(compare_vec4(got, expected, 0),
                    "Got {%.8e, %.8e, %.8e, %.8e}, expected {%.8e, %.8e, %.8e, %.8e}.\n",
                    got->x, got->y, got->z, got->w, expected->x, expected->y, expected->z, expected->w);
        }
    }
    release_resource_readback(&rb);

    ID3D12PipelineState_Release(context.pipeline_state);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    /* ps_gather4_green */
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, NULL, &ps_gather4_green, NULL);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, &white.x, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gpu_handle);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &constants.width, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < rb.height; ++y)
    {
        for (x = 0; x < rb.width; ++x)
        {
            const struct vec4 *expected = &expected_gather4_green[y * rb.width + x];
            const struct vec4 *got = get_readback_vec4(&rb, x, y);
            ok(compare_vec4(got, expected, 0),
                    "Got {%.8e, %.8e, %.8e, %.8e}, expected {%.8e, %.8e, %.8e, %.8e}.\n",
                    got->x, got->y, got->z, got->w, expected->x, expected->y, expected->z, expected->w);
        }
    }
    release_resource_readback(&rb);

    ID3D12PipelineState_Release(context.pipeline_state);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    /* ps_gather4_po */
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, NULL, &ps_gather4_po, NULL);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, &white.x, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gpu_handle);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &constants.width, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < rb.height; ++y)
    {
        for (x = 0; x < rb.width; ++x)
        {
            const struct vec4 *expected = &expected_gather4_offset[y * rb.width + x];
            const struct vec4 *got = get_readback_vec4(&rb, x, y);
            todo(compare_vec4(got, expected, 0),
                    "Got {%.8e, %.8e, %.8e, %.8e}, expected {%.8e, %.8e, %.8e, %.8e}.\n",
                    got->x, got->y, got->z, got->w, expected->x, expected->y, expected->z, expected->w);
        }
    }
    release_resource_readback(&rb);

    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    constants.offset_x = 0;
    constants.offset_y = 0;

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, &white.x, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gpu_handle);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, &constants.width, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < rb.height; ++y)
    {
        for (x = 0; x < rb.width; ++x)
        {
            const struct vec4 *expected = &expected_gather4[y * rb.width + x];
            const struct vec4 *got = get_readback_vec4(&rb, x, y);
            todo(compare_vec4(got, expected, 0),
                    "Got {%.8e, %.8e, %.8e, %.8e}, expected {%.8e, %.8e, %.8e, %.8e}.\n",
                    got->x, got->y, got->z, got->w, expected->x, expected->y, expected->z, expected->w);
        }
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(texture);
    ID3D12DescriptorHeap_Release(heap);
    destroy_test_context(&context);
}

static void test_descriptor_tables(void)
{
    ID3D12DescriptorHeap *heap, *sampler_heap, *heaps[2];
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_range[4];
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
    D3D12_ROOT_PARAMETER root_parameters[3];
    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12Resource *cb, *textures[4];
    unsigned int i, descriptor_size;
    D3D12_SAMPLER_DESC sampler_desc;
    struct test_context_desc desc;
    D3D12_SUBRESOURCE_DATA data;
    struct test_context context;
    ID3D12CommandQueue *queue;
    HRESULT hr;

    static const DWORD ps_code[] =
    {
#if 0
        Texture2D t0;
        Texture2D t1;
        Texture2D t2;
        Texture2D t3;
        SamplerState s0;

        cbuffer cb0
        {
            float4 c;
        };

        float4 main(float4 position : SV_POSITION) : SV_Target
        {
            float2 p = float2(position.x / 32.0f, position.y / 32.0f);

            return c.x * t0.Sample(s0, p) + c.y * t1.Sample(s0, p)
                    + c.z * t2.Sample(s0, p) + c.w * t3.Sample(s0, p);
        }
#endif
        0x43425844, 0xf848ef5f, 0x4da3fe0c, 0x776883a0, 0x6b3f0297, 0x00000001, 0x0000029c, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x5449534f, 0x004e4f49,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000200, 0x00000050,
        0x00000080, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300005a, 0x00106000,
        0x00000000, 0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x04001858, 0x00107000, 0x00000001,
        0x00005555, 0x04001858, 0x00107000, 0x00000002, 0x00005555, 0x04001858, 0x00107000, 0x00000003,
        0x00005555, 0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000003, 0x0a000038, 0x00100032, 0x00000000, 0x00101046, 0x00000000, 0x00004002,
        0x3d000000, 0x3d000000, 0x00000000, 0x00000000, 0x8b000045, 0x800000c2, 0x00155543, 0x001000f2,
        0x00000001, 0x00100046, 0x00000000, 0x00107e46, 0x00000001, 0x00106000, 0x00000000, 0x08000038,
        0x001000f2, 0x00000001, 0x00100e46, 0x00000001, 0x00208556, 0x00000000, 0x00000000, 0x8b000045,
        0x800000c2, 0x00155543, 0x001000f2, 0x00000002, 0x00100046, 0x00000000, 0x00107e46, 0x00000000,
        0x00106000, 0x00000000, 0x0a000032, 0x001000f2, 0x00000001, 0x00208006, 0x00000000, 0x00000000,
        0x00100e46, 0x00000002, 0x00100e46, 0x00000001, 0x8b000045, 0x800000c2, 0x00155543, 0x001000f2,
        0x00000002, 0x00100046, 0x00000000, 0x00107e46, 0x00000002, 0x00106000, 0x00000000, 0x8b000045,
        0x800000c2, 0x00155543, 0x001000f2, 0x00000000, 0x00100046, 0x00000000, 0x00107e46, 0x00000003,
        0x00106000, 0x00000000, 0x0a000032, 0x001000f2, 0x00000001, 0x00208aa6, 0x00000000, 0x00000000,
        0x00100e46, 0x00000002, 0x00100e46, 0x00000001, 0x0a000032, 0x001020f2, 0x00000000, 0x00208ff6,
        0x00000000, 0x00000000, 0x00100e46, 0x00000000, 0x00100e46, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const struct vec4 constant = {0.1f, 0.2f, 0.3f, 0.1f};
    static const unsigned int texture_data[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffff00};

    memset(&desc, 0, sizeof(desc));
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    cb = create_upload_buffer(context.device, sizeof(constant), &constant.x);

    descriptor_range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range[0].NumDescriptors = 2;
    descriptor_range[0].BaseShaderRegister = 0;
    descriptor_range[0].RegisterSpace = 0;
    descriptor_range[0].OffsetInDescriptorsFromTableStart = 1;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &descriptor_range[0];
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    descriptor_range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    descriptor_range[1].NumDescriptors = 1;
    descriptor_range[1].BaseShaderRegister = 0;
    descriptor_range[1].RegisterSpace = 0;
    descriptor_range[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &descriptor_range[1];
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    descriptor_range[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range[2].NumDescriptors = 2;
    descriptor_range[2].BaseShaderRegister = 2;
    descriptor_range[2].RegisterSpace = 0;
    descriptor_range[2].OffsetInDescriptorsFromTableStart = 0;
    descriptor_range[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    descriptor_range[3].NumDescriptors = 1;
    descriptor_range[3].BaseShaderRegister = 0;
    descriptor_range[3].RegisterSpace = 0;
    descriptor_range[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[2].DescriptorTable.NumDescriptorRanges = 2;
    root_parameters[2].DescriptorTable.pDescriptorRanges = &descriptor_range[2];
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 3;
    root_signature_desc.pParameters = root_parameters;
    hr = create_root_signature(context.device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps, NULL);

    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 6;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap_desc.NumDescriptors = 1;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&sampler_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(context.device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (i = 0; i < ARRAY_SIZE(textures); ++i)
    {
        textures[i] = create_texture(context.device,
                1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST);
        data.pData = &texture_data[i];
        data.RowPitch = sizeof(texture_data[i]);
        data.SlicePitch = data.RowPitch;
        upload_texture_data(textures[i], &data, 1, queue, command_list);
        reset_command_list(command_list, context.allocator);
    }

    for (i = 0; i < ARRAY_SIZE(textures); ++i)
        transition_resource_state(command_list, textures[i],
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    cpu_handle.ptr += descriptor_size;
    /* t0-t3 */
    for (i = 0; i < ARRAY_SIZE(textures); ++i)
    {
        ID3D12Device_CreateShaderResourceView(context.device, textures[i], NULL, cpu_handle);
        cpu_handle.ptr += descriptor_size;
    }
    /* cbv0 */
    cbv_desc.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(cb);
    cbv_desc.SizeInBytes = align(sizeof(constant), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    ID3D12Device_CreateConstantBufferView(context.device, &cbv_desc, cpu_handle);

    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(sampler_heap);
    /* s0 */
    ID3D12Device_CreateSampler(context.device, &sampler_desc, cpu_handle);

    gpu_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    heaps[0] = heap; heaps[1] = sampler_heap;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, ARRAY_SIZE(heaps), heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gpu_handle);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 1,
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(sampler_heap));
    gpu_handle.ptr += 3 * descriptor_size;
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 2, gpu_handle);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target, 0, queue, command_list, 0xb2664c19, 2);

    ID3D12Resource_Release(cb);
    for (i = 0; i < ARRAY_SIZE(textures); ++i)
        ID3D12Resource_Release(textures[i]);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12DescriptorHeap_Release(sampler_heap);
    destroy_test_context(&context);
}

/* Tests overlapping descriptor heap ranges for SRV and UAV descriptor tables.
 * Only descriptors used by the pipeline have to be valid.
 */
static void test_descriptor_tables_overlapping_bindings(void)
{
    ID3D12Resource *input_buffers[2], *output_buffers[2];
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_range[2];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D12_ROOT_PARAMETER root_parameters[3];
    ID3D12GraphicsCommandList *command_list;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    struct resource_readback rb;
    struct test_context context;
    ID3D12DescriptorHeap *heap;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        ByteAddressBuffer t0;
        ByteAddressBuffer t4 : register(t4);

        RWByteAddressBuffer u0;
        RWByteAddressBuffer u2 : register(u2);

        uint size;
        uint size2;

        [numthreads(1, 1, 1)]
        void main()
        {
            uint i;
            for (i = 0; i < size; ++i)
                u0.Store(4 * i, t0.Load(4 *i));
            for (i = 0; i < size2; ++i)
                u2.Store(4 * i, t4.Load(4 * i));
        }
#endif
        0x43425844, 0x8d2646b7, 0xeb60d9ee, 0x33ccd6ed, 0x5557e649, 0x00000001, 0x0000023c, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000001e8, 0x00050050, 0x0000007a, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x030000a1, 0x00107000, 0x00000000, 0x030000a1,
        0x00107000, 0x00000004, 0x0300009d, 0x0011e000, 0x00000000, 0x0300009d, 0x0011e000, 0x00000002,
        0x02000068, 0x00000001, 0x0400009b, 0x00000001, 0x00000001, 0x00000001, 0x05000036, 0x00100012,
        0x00000000, 0x00004001, 0x00000000, 0x01000030, 0x08000050, 0x00100022, 0x00000000, 0x0010000a,
        0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x03040003, 0x0010001a, 0x00000000, 0x07000029,
        0x00100022, 0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x00000002, 0x890000a5, 0x800002c2,
        0x00199983, 0x00100042, 0x00000000, 0x0010001a, 0x00000000, 0x00107006, 0x00000000, 0x070000a6,
        0x0011e012, 0x00000000, 0x0010001a, 0x00000000, 0x0010002a, 0x00000000, 0x0700001e, 0x00100012,
        0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x00000001, 0x01000016, 0x05000036, 0x00100012,
        0x00000000, 0x00004001, 0x00000000, 0x01000030, 0x08000050, 0x00100022, 0x00000000, 0x0010000a,
        0x00000000, 0x0020801a, 0x00000000, 0x00000000, 0x03040003, 0x0010001a, 0x00000000, 0x07000029,
        0x00100022, 0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x00000002, 0x890000a5, 0x800002c2,
        0x00199983, 0x00100042, 0x00000000, 0x0010001a, 0x00000000, 0x00107006, 0x00000004, 0x070000a6,
        0x0011e012, 0x00000002, 0x0010001a, 0x00000000, 0x0010002a, 0x00000000, 0x0700001e, 0x00100012,
        0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x00000001, 0x01000016, 0x0100003e,
    };
    static const uint32_t buffer_data[] = {0xdeadbabe};
    static const uint32_t buffer_data2[] = {0, 1, 2, 3, 4, 5};

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range[0].NumDescriptors = 10;
    descriptor_range[0].BaseShaderRegister = 0;
    descriptor_range[0].RegisterSpace = 0;
    descriptor_range[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &descriptor_range[0];
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    descriptor_range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_range[1].NumDescriptors = 10;
    descriptor_range[1].BaseShaderRegister = 0;
    descriptor_range[1].RegisterSpace = 0;
    descriptor_range[1].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &descriptor_range[1];
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[2].Constants.ShaderRegister = 0;
    root_parameters[2].Constants.RegisterSpace = 0;
    root_parameters[2].Constants.Num32BitValues = 2;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 3;
    root_signature_desc.pParameters = root_parameters;
    hr = create_root_signature(context.device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 30;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    input_buffers[0] = create_default_buffer(device, sizeof(buffer_data),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(input_buffers[0], 0, sizeof(buffer_data), &buffer_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, input_buffers[0],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    input_buffers[1] = create_default_buffer(device, sizeof(buffer_data2),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(input_buffers[1], 0, sizeof(buffer_data2), &buffer_data2, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, input_buffers[1],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    output_buffers[0] = create_default_buffer(device, sizeof(buffer_data),
              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    output_buffers[1] = create_default_buffer(device, sizeof(buffer_data2),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = ARRAY_SIZE(buffer_data);
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(device, output_buffers[0], NULL, &uav_desc,
            get_cpu_descriptor_handle(&context, heap, 0)); /* u0 */
    uav_desc.Buffer.NumElements = ARRAY_SIZE(buffer_data2);
    ID3D12Device_CreateUnorderedAccessView(device, output_buffers[1], NULL, &uav_desc,
            get_cpu_descriptor_handle(&context, heap, 2)); /* u2 */

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.FirstElement = 0;
    srv_desc.Buffer.NumElements = ARRAY_SIZE(buffer_data);
    srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    ID3D12Device_CreateShaderResourceView(device, input_buffers[0], &srv_desc,
            get_cpu_descriptor_handle(&context, heap, 3)); /* t0 */
    srv_desc.Buffer.NumElements = ARRAY_SIZE(buffer_data2);
    ID3D12Device_CreateShaderResourceView(device, input_buffers[1], &srv_desc,
            get_cpu_descriptor_handle(&context, heap, 7)); /* t4 */

    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0,
            get_gpu_descriptor_handle(&context, heap, 3));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 1,
            get_gpu_descriptor_handle(&context, heap, 0));
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(command_list, 2,
            ARRAY_SIZE(buffer_data), 0);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(command_list, 2,
            ARRAY_SIZE(buffer_data2), 1);
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    for (i = 0; i < ARRAY_SIZE(output_buffers); ++i)
    {
        transition_resource_state(command_list, output_buffers[i],
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    get_buffer_readback_with_command_list(output_buffers[0], DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(buffer_data); ++i)
    {
        unsigned int value = get_readback_uint(&rb, i, 0);
        ok(value == buffer_data[i], "Got %#x, expected %#x.\n", value, buffer_data[i]);
    }
    release_resource_readback(&rb);
    reset_command_list(command_list, context.allocator);
    get_buffer_readback_with_command_list(output_buffers[1], DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(buffer_data2); ++i)
    {
        unsigned int value = get_readback_uint(&rb, i, 0);
        ok(value == buffer_data2[i], "Got %#x, expected %#x.\n", value, buffer_data2[i]);
    }
    release_resource_readback(&rb);

    for (i = 0; i < ARRAY_SIZE(input_buffers); ++i)
        ID3D12Resource_Release(input_buffers[i]);
    for (i = 0; i < ARRAY_SIZE(output_buffers); ++i)
        ID3D12Resource_Release(output_buffers[i]);
    ID3D12DescriptorHeap_Release(heap);
    destroy_test_context(&context);
}

static void test_update_root_descriptors(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_GPU_VIRTUAL_ADDRESS cb_va, uav_va;
    D3D12_ROOT_PARAMETER root_parameters[2];
    ID3D12GraphicsCommandList *command_list;
    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;
    ID3D12Resource *resource, *cb;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        cbuffer cb
        {
            unsigned int offset;
            unsigned int value;
        };

        RWByteAddressBuffer b;

        [numthreads(1, 1, 1)]
        void main()
        {
            b.Store(4 * offset, value);
        }
#endif
        0x43425844, 0xaadc5460, 0x88c27e90, 0x2acacf4e, 0x4e06019a, 0x00000001, 0x000000d8, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000084, 0x00050050, 0x00000021, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300009d, 0x0011e000, 0x00000000, 0x02000068,
        0x00000001, 0x0400009b, 0x00000001, 0x00000001, 0x00000001, 0x08000029, 0x00100012, 0x00000000,
        0x0020800a, 0x00000000, 0x00000000, 0x00004001, 0x00000002, 0x080000a6, 0x0011e012, 0x00000000,
        0x0010000a, 0x00000000, 0x0020801a, 0x00000000, 0x00000000, 0x0100003e,
    };
    struct
    {
        uint32_t offset;
        uint32_t value;
        uint32_t uav_offset;
        uint8_t padding[D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 3 * sizeof(uint32_t)];
    }
    input[] =
    {
        {0, 4,  0},
        {2, 6,  0},
        {0, 5, 64},
        {7, 2, 64},
    };

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    cb = create_upload_buffer(context.device, sizeof(input), input);
    cb_va = ID3D12Resource_GetGPUVirtualAddress(cb);

    resource = create_default_buffer(device, 512,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uav_va = ID3D12Resource_GetGPUVirtualAddress(resource);

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[0].Descriptor.RegisterSpace = 0;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    pipeline_state = create_compute_pipeline_state(device, root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
    for (i = 0; i < ARRAY_SIZE(input); ++i)
    {
        ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list,
                0, cb_va + i * sizeof(*input));
        if (!i || input[i - 1].uav_offset != input[i].uav_offset)
            ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(command_list,
                    1, uav_va + input[i].uav_offset * sizeof(uint32_t));
        ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);
    }

    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(resource, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(input); ++i)
    {
        unsigned int offset = input[i].uav_offset + input[i].offset;
        unsigned int value = get_readback_uint(&rb, offset, 0);
        ok(value == input[i].value, "Got %#x, expected %#x.\n", value, input[i].value);
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(cb);
    ID3D12Resource_Release(resource);
    ID3D12RootSignature_Release(root_signature);
    ID3D12PipelineState_Release(pipeline_state);
    destroy_test_context(&context);
}

static void test_update_descriptor_tables(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_range;
    ID3D12GraphicsCommandList *command_list;
    D3D12_STATIC_SAMPLER_DESC sampler_desc;
    ID3D12DescriptorHeap *heap, *cpu_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    D3D12_ROOT_PARAMETER root_parameter;
    struct test_context_desc desc;
    D3D12_SUBRESOURCE_DATA data;
    struct resource_readback rb;
    struct test_context context;
    ID3D12Resource *textures[3];
    ID3D12CommandQueue *queue;
    unsigned int i;
    HRESULT hr;
    RECT rect;

    static const DWORD ps_code[] =
    {
#if 0
        Texture2D t0;
        Texture2D t1;
        SamplerState s;

        float4 main(float4 position : SV_POSITION) : SV_Target
        {
            float2 p = (position.x / 32.0f, position.x / 32.0f);
            return float4(t0.Sample(s, p).r, t1.Sample(s, p).r, 0, 1);
        }
#endif
        0x43425844, 0x5c19caa6, 0xd4fadb4f, 0xc9d6831e, 0x563b68b7, 0x00000001, 0x000001a4, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000010f, 0x505f5653, 0x5449534f, 0x004e4f49,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000108, 0x00000050,
        0x00000042, 0x0100086a, 0x0300005a, 0x00106000, 0x00000000, 0x04001858, 0x00107000, 0x00000000,
        0x00005555, 0x04001858, 0x00107000, 0x00000001, 0x00005555, 0x04002064, 0x00101012, 0x00000000,
        0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x07000038, 0x00100012,
        0x00000000, 0x0010100a, 0x00000000, 0x00004001, 0x3d000000, 0x8b000045, 0x800000c2, 0x00155543,
        0x00100022, 0x00000000, 0x00100006, 0x00000000, 0x00107e16, 0x00000000, 0x00106000, 0x00000000,
        0x8b000045, 0x800000c2, 0x00155543, 0x00100012, 0x00000000, 0x00100006, 0x00000000, 0x00107e46,
        0x00000001, 0x00106000, 0x00000000, 0x05000036, 0x00102032, 0x00000000, 0x00100516, 0x00000000,
        0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x3f800000,
        0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const float texture_data[] = {0.5f, 0.25f, 0.1f};

    memset(&desc, 0, sizeof(desc));
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.ShaderRegister = 0;
    sampler_desc.RegisterSpace = 0;
    sampler_desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range.NumDescriptors = 2;
    descriptor_range.BaseShaderRegister = 0;
    descriptor_range.RegisterSpace = 0;
    descriptor_range.OffsetInDescriptorsFromTableStart = 0;
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameter.DescriptorTable.NumDescriptorRanges = 1;
    root_parameter.DescriptorTable.pDescriptorRanges = &descriptor_range;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = &root_parameter;
    root_signature_desc.NumStaticSamplers = 1;
    root_signature_desc.pStaticSamplers = &sampler_desc;
    hr = create_root_signature(context.device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps, NULL);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 4;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cpu_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    for (i = 0; i < ARRAY_SIZE(textures); ++i)
    {
        textures[i] = create_texture(context.device, 1, 1, DXGI_FORMAT_R32_FLOAT,
                D3D12_RESOURCE_STATE_COPY_DEST);
        data.pData = &texture_data[i];
        data.RowPitch = sizeof(texture_data[i]);
        data.SlicePitch = data.RowPitch;
        upload_texture_data(textures[i], &data, 1, queue, command_list);
        reset_command_list(command_list, context.allocator);
        transition_resource_state(command_list, textures[i],
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    for (i = 0; i < heap_desc.NumDescriptors; ++i)
    {
        ID3D12Device_CreateShaderResourceView(context.device, textures[2], NULL,
                get_cpu_descriptor_handle(&context, heap, i));
    }
    for (i = 0; i < ARRAY_SIZE(textures); ++i)
    {
        ID3D12Device_CreateShaderResourceView(context.device, textures[i], NULL,
                get_cpu_descriptor_handle(&context, cpu_heap, i));
    }

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);

    set_rect(&rect, 0, 0, 16, 32);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &rect);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0,
            get_gpu_descriptor_handle(&context, heap, 0));
    ID3D12Device_CopyDescriptorsSimple(context.device, 2,
            get_cpu_sampler_handle(&context, heap, 0),
            get_cpu_sampler_handle(&context, cpu_heap, 0),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    set_rect(&rect, 16, 0, 32, 32);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &rect);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0,
            get_gpu_descriptor_handle(&context, heap, 2));
    ID3D12Device_CreateShaderResourceView(context.device, textures[1], NULL,
            get_cpu_descriptor_handle(&context, heap, 2));
    ID3D12Device_CreateShaderResourceView(context.device, textures[0], NULL,
            get_cpu_descriptor_handle(&context, heap, 3));
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    set_rect(&rect, 0, 0, 16, 32);
    check_readback_data_uint(&rb, &rect, 0xff00407f, 1);
    set_rect(&rect, 16, 0, 32, 32);
    check_readback_data_uint(&rb, &rect, 0xff007f40, 1);
    release_resource_readback(&rb);

    for (i = 0; i < ARRAY_SIZE(textures); ++i)
        ID3D12Resource_Release(textures[i]);
    ID3D12DescriptorHeap_Release(cpu_heap);
    ID3D12DescriptorHeap_Release(heap);
    destroy_test_context(&context);
}

/* This cannot be implemented reasonably in Vulkan. Vulkan doesn't allow
 * updating descriptor sets after the vkCmdBindDescriptorSets() command
 * is recorded.
 */
static void test_update_descriptor_heap_after_closing_command_list(void)
{
    ID3D12Resource *red_texture, *green_texture;
    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    ID3D12DescriptorHeap *cpu_heap, *heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    D3D12_SUBRESOURCE_DATA texture_data;
    struct test_context_desc desc;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    unsigned int value;
    HRESULT hr;

    static const DWORD ps_code[] =
    {
#if 0
        Texture2D t;
        SamplerState s;

        float4 main(float4 position : SV_POSITION) : SV_Target
        {
            float2 p;

            p.x = position.x / 32.0f;
            p.y = position.y / 32.0f;
            return t.Sample(s, p);
        }
#endif
        0x43425844, 0x7a0c3929, 0x75ff3ca4, 0xccb318b2, 0xe6965b4c, 0x00000001, 0x00000140, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x5449534f, 0x004e4f49,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000a4, 0x00000050,
        0x00000029, 0x0100086a, 0x0300005a, 0x00106000, 0x00000000, 0x04001858, 0x00107000, 0x00000000,
        0x00005555, 0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x0a000038, 0x00100032, 0x00000000, 0x00101046, 0x00000000, 0x00004002,
        0x3d000000, 0x3d000000, 0x00000000, 0x00000000, 0x8b000045, 0x800000c2, 0x00155543, 0x001020f2,
        0x00000000, 0x00100046, 0x00000000, 0x00107e46, 0x00000000, 0x00106000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const unsigned int red_data[] = {0xff0000ff};
    static const unsigned int green_data[] = {0xff00ff00};

    memset(&desc, 0, sizeof(desc));
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_texture_root_signature(context.device,
            D3D12_SHADER_VISIBILITY_PIXEL, 0, 0);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps, NULL);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);

    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cpu_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    red_texture = create_texture(context.device, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_STATE_COPY_DEST);
    texture_data.pData = red_data;
    texture_data.RowPitch = sizeof(*red_data);
    texture_data.SlicePitch = texture_data.RowPitch;
    upload_texture_data(red_texture, &texture_data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, red_texture,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    green_texture = create_texture(context.device, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_STATE_COPY_DEST);
    texture_data.pData = green_data;
    upload_texture_data(green_texture, &texture_data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, green_texture,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ID3D12Device_CreateShaderResourceView(context.device, red_texture, NULL,
            get_cpu_descriptor_handle(&context, cpu_heap, 0));
    ID3D12Device_CopyDescriptorsSimple(context.device, 1,
            get_cpu_sampler_handle(&context, heap, 0),
            get_cpu_sampler_handle(&context, cpu_heap, 0),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0,
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap));
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Failed to close command list, hr %#x.\n", hr);

    /* Update the descriptor heap used by the closed command list. */
    ID3D12Device_CreateShaderResourceView(context.device, green_texture, NULL, cpu_handle);

    exec_command_list(queue, command_list);
    wait_queue_idle(context.device, queue);
    reset_command_list(command_list, context.allocator);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    value = get_readback_uint(&rb, 0, 0);
    todo(value == 0xff00ff00, "Got unexpected value %#x.\n", value);
    release_resource_readback(&rb);

    ID3D12DescriptorHeap_Release(cpu_heap);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12Resource_Release(green_texture);
    ID3D12Resource_Release(red_texture);
    destroy_test_context(&context);
}

static void test_update_compute_descriptor_tables(void)
{
    struct cb_data
    {
        struct uvec4 srv_size[2];
        struct uvec4 uav_size[2];
    };

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12PipelineState *buffer_pso, *texture_pso;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[4];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[5];
    D3D12_SUBRESOURCE_DATA subresource_data;
    ID3D12Resource *buffer_cb, *texture_cb;
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12Resource *output_buffers[2];
    ID3D12Resource *input_buffers[5];
    ID3D12Resource *textures[3];
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    struct cb_data cb_data;
    ID3D12Device *device;
    unsigned int i;
    uint32_t data;
    HRESULT hr;

    static const DWORD cs_buffer_code[] =
    {
#if 0
        uint offset;

        RWByteAddressBuffer u0 : register(u0);

        cbuffer cb0 : register(b0)
        {
            uint4 srv_size[2];
            uint4 uav_size[2];
        };

        Buffer<uint> t0 : register(t0);
        Buffer<uint> t1 : register(t1);

        RWBuffer<uint> u4 : register(u4);
        RWBuffer<uint> u7 : register(u7);

        [numthreads(1, 1, 1)]
        void main()
        {
            uint x, result, byte_offset = offset;

            for (x = 0, result = 0; x < srv_size[0].x; ++x)
                result += t0.Load(x);
            u0.Store(byte_offset, result);
            byte_offset += 4;

            for (x = 0, result = 0; x < srv_size[1].x; ++x)
                result += t1.Load(x);
            u0.Store(byte_offset, result);
            byte_offset += 4;

            for (x = 0, result = 0; x < uav_size[0].x; ++x)
                result += u4[x];
            u0.Store(byte_offset, result);
            byte_offset += 4;

            for (x = 0, result = 0; x < uav_size[1].x; ++x)
                result += u7[x];
            u0.Store(byte_offset, result);
        }
#endif
        0x43425844, 0xb3d9f052, 0xcc3f0310, 0xd18f8515, 0xccabd8f6, 0x00000001, 0x00000404, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000003b0, 0x00050050, 0x000000ec, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000001, 0x00000001, 0x04000059, 0x00208e46, 0x00000000, 0x00000004,
        0x04000858, 0x00107000, 0x00000000, 0x00004444, 0x04000858, 0x00107000, 0x00000001, 0x00004444,
        0x0300009d, 0x0011e000, 0x00000000, 0x0400089c, 0x0011e000, 0x00000004, 0x00004444, 0x0400089c,
        0x0011e000, 0x00000007, 0x00004444, 0x02000068, 0x00000002, 0x0400009b, 0x00000001, 0x00000001,
        0x00000001, 0x08000036, 0x00100032, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x01000030, 0x08000050, 0x00100012, 0x00000001, 0x0010001a, 0x00000000, 0x0020800a,
        0x00000000, 0x00000000, 0x03040003, 0x0010000a, 0x00000001, 0x8900002d, 0x80000042, 0x00111103,
        0x00100012, 0x00000001, 0x00100556, 0x00000000, 0x00107e46, 0x00000000, 0x0700001e, 0x00100012,
        0x00000000, 0x0010000a, 0x00000000, 0x0010000a, 0x00000001, 0x0700001e, 0x00100022, 0x00000000,
        0x0010001a, 0x00000000, 0x00004001, 0x00000001, 0x01000016, 0x08000036, 0x00100032, 0x00000001,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000030, 0x08000050, 0x00100042,
        0x00000001, 0x0010000a, 0x00000001, 0x0020800a, 0x00000000, 0x00000001, 0x03040003, 0x0010002a,
        0x00000001, 0x8900002d, 0x80000042, 0x00111103, 0x00100042, 0x00000001, 0x00100006, 0x00000001,
        0x00107c96, 0x00000001, 0x0700001e, 0x00100022, 0x00000001, 0x0010001a, 0x00000001, 0x0010002a,
        0x00000001, 0x0700001e, 0x00100012, 0x00000001, 0x0010000a, 0x00000001, 0x00004001, 0x00000001,
        0x01000016, 0x05000036, 0x00100022, 0x00000000, 0x0010001a, 0x00000001, 0x08000036, 0x00100032,
        0x00000001, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000030, 0x08000050,
        0x00100042, 0x00000001, 0x0010000a, 0x00000001, 0x0020800a, 0x00000000, 0x00000002, 0x03040003,
        0x0010002a, 0x00000001, 0x890000a3, 0x80000042, 0x00111103, 0x00100042, 0x00000001, 0x00100006,
        0x00000001, 0x0011ec96, 0x00000004, 0x0700001e, 0x00100022, 0x00000001, 0x0010001a, 0x00000001,
        0x0010002a, 0x00000001, 0x0700001e, 0x00100012, 0x00000001, 0x0010000a, 0x00000001, 0x00004001,
        0x00000001, 0x01000016, 0x05000036, 0x00100042, 0x00000000, 0x0010001a, 0x00000001, 0x08000036,
        0x00100032, 0x00000001, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000030,
        0x08000050, 0x00100042, 0x00000001, 0x0010000a, 0x00000001, 0x0020800a, 0x00000000, 0x00000003,
        0x03040003, 0x0010002a, 0x00000001, 0x890000a3, 0x80000042, 0x00111103, 0x00100042, 0x00000001,
        0x00100006, 0x00000001, 0x0011ec96, 0x00000007, 0x0700001e, 0x00100022, 0x00000001, 0x0010001a,
        0x00000001, 0x0010002a, 0x00000001, 0x0700001e, 0x00100012, 0x00000001, 0x0010000a, 0x00000001,
        0x00004001, 0x00000001, 0x01000016, 0x05000036, 0x00100082, 0x00000000, 0x0010001a, 0x00000001,
        0x080000a6, 0x0011e0f2, 0x00000000, 0x0020800a, 0x00000001, 0x00000000, 0x00100e46, 0x00000000,
        0x0100003e,
    };
    static const DWORD cs_texture_code[] =
    {
#if 0
        uint offset;

        RWByteAddressBuffer u0 : register(u0);

        cbuffer cb0 : register(b0)
        {
            uint4 srv_size[2];
            uint4 uav_size[2];
        };

        Texture2D<uint> t0 : register(t0);
        Texture2D<uint> t1 : register(t1);

        RWBuffer<uint> u4 : register(u4);
        RWTexture2D<uint> u6 : register(u6);

        [numthreads(1, 1, 1)]
        void main()
        {
            uint x, y, result, byte_offset = offset;

            for (y = 0, result = 0; y < srv_size[0].y; ++y)
            {
                for (x = 0; x < srv_size[0].x; ++x)
                    result += t0.Load(int3(x, y, 0));
            }
            u0.Store(byte_offset, result);
            byte_offset += 4;

            for (y = 0, result = 0; y < srv_size[1].y; ++y)
            {
                for (x = 0; x < srv_size[1].x; ++x)
                    result += t1.Load(int3(x, y, 0));
            }
            u0.Store(byte_offset, result);
            byte_offset += 4;

            for (x = 0, result = 0; x < uav_size[0].x; ++x)
                result += u4[x];
            u0.Store(byte_offset, result);
            byte_offset += 4;

            for (y = 0, result = 0; y < uav_size[1].y; ++y)
            {
                for (x = 0; x < uav_size[1].x; ++x)
                    result += u6[uint2(x, y)];
            }
            u0.Store(byte_offset, result);
        }
#endif
        0x43425844, 0x3f0f012e, 0xfb75f6aa, 0xb87ffe68, 0xf25f9ee6, 0x00000001, 0x00000650, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000005fc, 0x00050050, 0x0000017f, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000001, 0x00000001, 0x04000059, 0x00208e46, 0x00000000, 0x00000004,
        0x04001858, 0x00107000, 0x00000000, 0x00004444, 0x04001858, 0x00107000, 0x00000001, 0x00004444,
        0x0300009d, 0x0011e000, 0x00000000, 0x0400089c, 0x0011e000, 0x00000004, 0x00004444, 0x0400189c,
        0x0011e000, 0x00000006, 0x00004444, 0x02000068, 0x00000004, 0x0400009b, 0x00000001, 0x00000001,
        0x00000001, 0x08000036, 0x001000c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x05000036, 0x00100012, 0x00000001, 0x00004001, 0x00000000, 0x05000036, 0x00100012,
        0x00000002, 0x00004001, 0x00000000, 0x01000030, 0x08000050, 0x00100022, 0x00000001, 0x0010000a,
        0x00000001, 0x0020801a, 0x00000000, 0x00000000, 0x03040003, 0x0010001a, 0x00000001, 0x05000036,
        0x00100022, 0x00000000, 0x0010000a, 0x00000001, 0x05000036, 0x00100012, 0x00000003, 0x00004001,
        0x00000000, 0x05000036, 0x00100022, 0x00000003, 0x0010000a, 0x00000002, 0x01000030, 0x08000050,
        0x00100022, 0x00000001, 0x0010000a, 0x00000003, 0x0020800a, 0x00000000, 0x00000000, 0x03040003,
        0x0010001a, 0x00000001, 0x05000036, 0x00100012, 0x00000000, 0x0010000a, 0x00000003, 0x8900002d,
        0x800000c2, 0x00111103, 0x00100012, 0x00000000, 0x00100e46, 0x00000000, 0x00107e46, 0x00000000,
        0x0700001e, 0x00100022, 0x00000003, 0x0010001a, 0x00000003, 0x0010000a, 0x00000000, 0x0700001e,
        0x00100012, 0x00000003, 0x0010000a, 0x00000003, 0x00004001, 0x00000001, 0x01000016, 0x05000036,
        0x00100012, 0x00000002, 0x0010001a, 0x00000003, 0x0700001e, 0x00100012, 0x00000001, 0x0010000a,
        0x00000001, 0x00004001, 0x00000001, 0x01000016, 0x08000036, 0x001000c2, 0x00000000, 0x00004002,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x05000036, 0x00100012, 0x00000001, 0x00004001,
        0x00000000, 0x05000036, 0x00100022, 0x00000002, 0x00004001, 0x00000000, 0x01000030, 0x08000050,
        0x00100022, 0x00000001, 0x0010000a, 0x00000001, 0x0020801a, 0x00000000, 0x00000001, 0x03040003,
        0x0010001a, 0x00000001, 0x05000036, 0x00100022, 0x00000000, 0x0010000a, 0x00000001, 0x05000036,
        0x00100012, 0x00000003, 0x00004001, 0x00000000, 0x05000036, 0x00100022, 0x00000003, 0x0010001a,
        0x00000002, 0x01000030, 0x08000050, 0x00100022, 0x00000001, 0x0010000a, 0x00000003, 0x0020800a,
        0x00000000, 0x00000001, 0x03040003, 0x0010001a, 0x00000001, 0x05000036, 0x00100012, 0x00000000,
        0x0010000a, 0x00000003, 0x8900002d, 0x800000c2, 0x00111103, 0x00100012, 0x00000000, 0x00100e46,
        0x00000000, 0x00107e46, 0x00000001, 0x0700001e, 0x00100022, 0x00000003, 0x0010001a, 0x00000003,
        0x0010000a, 0x00000000, 0x0700001e, 0x00100012, 0x00000003, 0x0010000a, 0x00000003, 0x00004001,
        0x00000001, 0x01000016, 0x05000036, 0x00100022, 0x00000002, 0x0010001a, 0x00000003, 0x0700001e,
        0x00100012, 0x00000001, 0x0010000a, 0x00000001, 0x00004001, 0x00000001, 0x01000016, 0x08000036,
        0x00100032, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000030,
        0x08000050, 0x00100042, 0x00000000, 0x0010000a, 0x00000000, 0x0020800a, 0x00000000, 0x00000002,
        0x03040003, 0x0010002a, 0x00000000, 0x890000a3, 0x80000042, 0x00111103, 0x00100042, 0x00000000,
        0x00100006, 0x00000000, 0x0011ec96, 0x00000004, 0x0700001e, 0x00100022, 0x00000000, 0x0010001a,
        0x00000000, 0x0010002a, 0x00000000, 0x0700001e, 0x00100012, 0x00000000, 0x0010000a, 0x00000000,
        0x00004001, 0x00000001, 0x01000016, 0x05000036, 0x00100042, 0x00000002, 0x0010001a, 0x00000000,
        0x05000036, 0x00100012, 0x00000000, 0x00004001, 0x00000000, 0x05000036, 0x00100082, 0x00000002,
        0x00004001, 0x00000000, 0x01000030, 0x08000050, 0x00100022, 0x00000000, 0x0010000a, 0x00000000,
        0x0020801a, 0x00000000, 0x00000003, 0x03040003, 0x0010001a, 0x00000000, 0x05000036, 0x001000e2,
        0x00000001, 0x00100006, 0x00000000, 0x05000036, 0x00100012, 0x00000003, 0x00004001, 0x00000000,
        0x05000036, 0x00100022, 0x00000003, 0x0010003a, 0x00000002, 0x01000030, 0x08000050, 0x00100022,
        0x00000000, 0x0010000a, 0x00000003, 0x0020800a, 0x00000000, 0x00000003, 0x03040003, 0x0010001a,
        0x00000000, 0x05000036, 0x00100012, 0x00000001, 0x0010000a, 0x00000003, 0x890000a3, 0x800000c2,
        0x00111103, 0x00100022, 0x00000000, 0x00100e46, 0x00000001, 0x0011ee16, 0x00000006, 0x0700001e,
        0x00100022, 0x00000003, 0x0010001a, 0x00000003, 0x0010001a, 0x00000000, 0x0700001e, 0x00100012,
        0x00000003, 0x0010000a, 0x00000003, 0x00004001, 0x00000001, 0x01000016, 0x05000036, 0x00100082,
        0x00000002, 0x0010001a, 0x00000003, 0x0700001e, 0x00100012, 0x00000000, 0x0010000a, 0x00000000,
        0x00004001, 0x00000001, 0x01000016, 0x080000a6, 0x0011e0f2, 0x00000000, 0x0020800a, 0x00000001,
        0x00000000, 0x00100e46, 0x00000002, 0x0100003e,
    };
    static const uint32_t buffer0_data[] = {1, 2, 3, 1};
    static const uint32_t buffer1_data[] = {10, 20, 30, 10};
    static const uint32_t buffer2_data[] = {100, 200, 300, 200};
    static const uint32_t buffer3_data[] = {1000, 2000, 2000, 2000};
    static const uint32_t buffer4_data[] = {0, 0, 0, 0};
    static const uint32_t texture0_data[4][4] =
    {
        {1, 0, 0, 0},
        {10000, 100, 1000, 10000},
        {0, 0, 0, 2},
        {0, 30000, 10000, 10},
    };
    static const uint32_t texture1_data[4][4] =
    {
        {6, 0, 0, 0},
        {600, 0, 1000, 60000},
        {0, 40, 0, 0},
        {0, 30000, 0, 0},
    };
    static const uint32_t texture2_data[4][4] =
    {
        {1, 1, 1, 1},
        {2, 2, 2, 2},
        {3, 3, 3, 3},
        {4, 4, 4, 4},
    };
    static const uint32_t expected_output0[] = {7, 70, 800, 7000, 70, 0, 800, 7000, 61113, 91646, 800, 40};
    static const uint32_t expected_output1[] = {61113, 91646, 800, 40, 7, 70, 800, 7000};

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].Constants.ShaderRegister = 1;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = 1;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &descriptor_ranges[0];
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[1].NumDescriptors = 2;
    descriptor_ranges[1].BaseShaderRegister = 0;
    descriptor_ranges[1].RegisterSpace = 0;
    descriptor_ranges[1].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[2].DescriptorTable.pDescriptorRanges = &descriptor_ranges[1];
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    descriptor_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[2].NumDescriptors = 4;
    descriptor_ranges[2].BaseShaderRegister = 4;
    descriptor_ranges[2].RegisterSpace = 0;
    descriptor_ranges[2].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[3].DescriptorTable.pDescriptorRanges = &descriptor_ranges[2];
    root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    descriptor_ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    descriptor_ranges[3].NumDescriptors = 1;
    descriptor_ranges[3].BaseShaderRegister = 0;
    descriptor_ranges[3].RegisterSpace = 0;
    descriptor_ranges[3].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[4].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[4].DescriptorTable.pDescriptorRanges = &descriptor_ranges[3];
    root_parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 5;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    buffer_pso = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_buffer_code, sizeof(cs_buffer_code)));
    texture_pso = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_texture_code, sizeof(cs_texture_code)));

    for (i = 0; i < ARRAY_SIZE(output_buffers); ++i)
    {
        output_buffers[i] = create_default_buffer(device, 1024,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    input_buffers[0] = create_default_buffer(device, sizeof(buffer0_data),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(input_buffers[0], 0, sizeof(buffer0_data), buffer0_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, input_buffers[0],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    input_buffers[1] = create_default_buffer(device, sizeof(buffer1_data),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(input_buffers[1], 0, sizeof(buffer1_data), buffer1_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, input_buffers[1],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    input_buffers[2] = create_default_buffer(device, sizeof(buffer2_data),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(input_buffers[2], 0, sizeof(buffer2_data), buffer2_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, input_buffers[2],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    input_buffers[3] = create_default_buffer(device, sizeof(buffer3_data),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(input_buffers[3], 0, sizeof(buffer3_data), buffer3_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, input_buffers[3],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    input_buffers[4] = create_default_buffer(device, sizeof(buffer4_data),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(input_buffers[4], 0, sizeof(buffer4_data), buffer4_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, input_buffers[4],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    textures[0] = create_texture(context.device,
            4, 4, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_STATE_COPY_DEST);
    subresource_data.pData = texture0_data;
    subresource_data.RowPitch = sizeof(*texture0_data);
    subresource_data.SlicePitch = subresource_data.RowPitch;
    upload_texture_data(textures[0], &subresource_data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, textures[0],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    textures[1] = create_texture(context.device,
            4, 4, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_STATE_COPY_DEST);
    subresource_data.pData = texture1_data;
    subresource_data.RowPitch = sizeof(*texture1_data);
    subresource_data.SlicePitch = subresource_data.RowPitch;
    upload_texture_data(textures[1], &subresource_data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, textures[1],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    textures[2] = create_default_texture(context.device, 4, 4, DXGI_FORMAT_R32_UINT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    subresource_data.pData = texture2_data;
    subresource_data.RowPitch = sizeof(*texture2_data);
    subresource_data.SlicePitch = subresource_data.RowPitch;
    upload_texture_data(textures[2], &subresource_data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, textures[2],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    memset(&cb_data, 0, sizeof(cb_data));
    cb_data.srv_size[0].x = ARRAY_SIZE(buffer0_data);
    cb_data.srv_size[1].x = ARRAY_SIZE(buffer1_data);
    cb_data.uav_size[0].x = ARRAY_SIZE(buffer2_data);
    cb_data.uav_size[1].x = ARRAY_SIZE(buffer3_data);
    buffer_cb = create_upload_buffer(device, sizeof(cb_data), &cb_data);

    memset(&cb_data, 0, sizeof(cb_data));
    cb_data.srv_size[0].x = 4;
    cb_data.srv_size[0].y = 4;
    cb_data.srv_size[1].x = 4;
    cb_data.srv_size[1].y = 4;
    cb_data.uav_size[0].x = ARRAY_SIZE(buffer2_data);
    cb_data.uav_size[1].x = 4;
    cb_data.uav_size[1].y = 4;
    texture_cb = create_upload_buffer(device, sizeof(cb_data), &cb_data);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 30;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R32_UINT;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.FirstElement = 0;
    srv_desc.Buffer.NumElements = ARRAY_SIZE(buffer0_data);
    ID3D12Device_CreateShaderResourceView(device, input_buffers[0], &srv_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 0));
    srv_desc.Buffer.NumElements = ARRAY_SIZE(buffer1_data);
    ID3D12Device_CreateShaderResourceView(device, input_buffers[1], &srv_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 1));

    ID3D12Device_CreateShaderResourceView(device, input_buffers[1], &srv_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 6));
    srv_desc.Buffer.NumElements = ARRAY_SIZE(buffer4_data);
    ID3D12Device_CreateShaderResourceView(device, input_buffers[4], &srv_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 7));

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.Format = DXGI_FORMAT_R32_UINT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = ARRAY_SIZE(buffer2_data);
    ID3D12Device_CreateUnorderedAccessView(device, input_buffers[2], NULL, &uav_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 2));
    ID3D12Device_CreateUnorderedAccessView(device, input_buffers[2], NULL, &uav_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 12));
    uav_desc.Buffer.NumElements = ARRAY_SIZE(buffer3_data);
    ID3D12Device_CreateUnorderedAccessView(device, input_buffers[3], NULL, &uav_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 5));

    ID3D12Device_CreateShaderResourceView(device, textures[0], NULL,
            get_cpu_descriptor_handle(&context, descriptor_heap, 10));
    ID3D12Device_CreateShaderResourceView(device, textures[1], NULL,
            get_cpu_descriptor_handle(&context, descriptor_heap, 11));

    ID3D12Device_CreateUnorderedAccessView(device, textures[2], NULL, NULL,
            get_cpu_descriptor_handle(&context, descriptor_heap, 14));

    cbv_desc.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buffer_cb);
    cbv_desc.SizeInBytes = align(sizeof(cb_data), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    ID3D12Device_CreateConstantBufferView(context.device, &cbv_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 8));

    cbv_desc.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(texture_cb);
    cbv_desc.SizeInBytes = align(sizeof(cb_data), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    ID3D12Device_CreateConstantBufferView(context.device, &cbv_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 9));

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = 256;
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(device, output_buffers[0], NULL, &uav_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 20));
    ID3D12Device_CreateUnorderedAccessView(device, output_buffers[1], NULL, &uav_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 21));

    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, buffer_pso);

    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(command_list, 0, 0 /* offset */, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            1, get_gpu_descriptor_handle(&context, descriptor_heap, 20)); /* u0 */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            2, get_gpu_descriptor_handle(&context, descriptor_heap, 0)); /* t1-t2 */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            3, get_gpu_descriptor_handle(&context, descriptor_heap, 2)); /* u4-u7 */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            4, get_gpu_descriptor_handle(&context, descriptor_heap, 8)); /* b0 */
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(command_list, 0, 16 /* offset */, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            2, get_gpu_descriptor_handle(&context, descriptor_heap, 6));  /* t1-t2 */
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, texture_pso);

    transition_resource_state(command_list, input_buffers[4],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(command_list, 0, 32 /* offset */, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            2, get_gpu_descriptor_handle(&context, descriptor_heap, 10)); /* t1-t2 */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            3, get_gpu_descriptor_handle(&context, descriptor_heap, 12)); /* u4-u7 */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            4, get_gpu_descriptor_handle(&context, descriptor_heap, 9)); /* b0 */
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(command_list, 0, 0 /* offset */, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            1, get_gpu_descriptor_handle(&context, descriptor_heap, 21)); /* u0 */
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, buffer_pso);

    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(command_list, 0, 16 /* offset */, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            2, get_gpu_descriptor_handle(&context, descriptor_heap, 0)); /* t1-t2 */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            3, get_gpu_descriptor_handle(&context, descriptor_heap, 2)); /* u4-u7 */
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            4, get_gpu_descriptor_handle(&context, descriptor_heap, 8)); /* b0 */
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    transition_sub_resource_state(command_list, output_buffers[0], 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(output_buffers[0], DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(expected_output0); ++i)
    {
        data = get_readback_uint(&rb, i, 0);
        ok(data == expected_output0[i], "Got %#x, expected %#x at %u.\n", data, expected_output0[i], i);
    }
    release_resource_readback(&rb);

    reset_command_list(command_list, context.allocator);
    transition_sub_resource_state(command_list, output_buffers[1], 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(output_buffers[1], DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(expected_output1); ++i)
    {
        data = get_readback_uint(&rb, i, 0);
        ok(data == expected_output1[i], "Got %#x, expected %#x at %u.\n", data, expected_output1[i], i);
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(buffer_cb);
    ID3D12Resource_Release(texture_cb);
    for (i = 0; i < ARRAY_SIZE(input_buffers); ++i)
        ID3D12Resource_Release(input_buffers[i]);
    for (i = 0; i < ARRAY_SIZE(textures); ++i)
        ID3D12Resource_Release(textures[i]);
    for (i = 0; i < ARRAY_SIZE(output_buffers); ++i)
        ID3D12Resource_Release(output_buffers[i]);
    ID3D12PipelineState_Release(buffer_pso);
    ID3D12PipelineState_Release(texture_pso);
    ID3D12DescriptorHeap_Release(descriptor_heap);
    destroy_test_context(&context);
}

static void test_copy_descriptors(void)
{
    struct data
    {
        unsigned int u[3];
        float f;
    };

    ID3D12DescriptorHeap *cpu_heap, *cpu_sampler_heap, *cpu_sampler_heap2;
    D3D12_CPU_DESCRIPTOR_HANDLE dst_handles[4], src_handles[4];
    ID3D12DescriptorHeap *heap, *sampler_heap, *heaps[2];
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[5];
    UINT dst_range_sizes[4], src_range_sizes[4];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[4];
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12Resource *t[7], *u[3], *cb;
    struct depth_stencil_resource ds;
    D3D12_SAMPLER_DESC sampler_desc;
    struct test_context_desc desc;
    unsigned int descriptor_size;
    D3D12_SUBRESOURCE_DATA data;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    unsigned int sampler_size;
    ID3D12Device *device;
    unsigned int *result;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        struct data
        {
            uint3 u;
            float f;
        };

        cbuffer cb0
        {
            float f;
        };

        cbuffer cb1
        {
            uint u;
        };

        cbuffer cb2
        {
            int i;
        };

        SamplerState s0;
        SamplerState s1;
        SamplerState s2;
        SamplerComparisonState s3;

        Texture2D t0;
        Texture2D<uint> t1;
        Texture2D<int> t2;
        Buffer<float> t3;
        StructuredBuffer<float> t4;
        ByteAddressBuffer t5;
        Texture2D t6;

        RWByteAddressBuffer u0;
        RWStructuredBuffer<data> u1;

        RWByteAddressBuffer u2;

        [numthreads(1, 1, 1)]
        void main()
        {
            u2.Store(0 * 4, f);
            u2.Store(1 * 4, u);
            u2.Store(2 * 4, i);
            u2.Store(3 * 4, 0);

            u2.Store4( 4 * 4, t0.SampleLevel(s0, (float2)0, 0));
            u2.Store4( 8 * 4, t0.SampleLevel(s1, (float2)0, 0));
            u2.Store4(12 * 4, t0.SampleLevel(s2, (float2)0, 0));

            u2.Store(16 * 4, t1.Load((int3)0));
            u2.Store(17 * 4, t2.Load((int3)0));
            u2.Store(18 * 4, t3.Load(0));
            u2.Store(19 * 4, t4[0]);

            u2.Store4(20 * 4, t5.Load4(0));

            u2.Store4(24 * 4, t6.SampleCmpLevelZero(s3, (float2)0, 0.6f));
            u2.Store4(28 * 4, t6.SampleCmpLevelZero(s3, (float2)0, 0.4f));

            u2.Store2(32 * 4, u0.Load2(0));
            u2.Store2(34 * 4, u0.Load2(8));

            u2.Store3(36 * 4, u1[0].u);
            u2.Store4(39 * 4, u1[0].f);

            u2.Store(43 * 4, 0xdeadbeef);
        }
#endif
        0x43425844, 0x52d2c2d3, 0xaf60e190, 0xb897944f, 0x4a6a6653, 0x00000001, 0x00000650, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000005fc, 0x00050050, 0x0000017f, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x04000059, 0x00208e46, 0x00000001, 0x00000001,
        0x04000059, 0x00208e46, 0x00000002, 0x00000001, 0x0300005a, 0x00106000, 0x00000000, 0x0300005a,
        0x00106000, 0x00000001, 0x0300005a, 0x00106000, 0x00000002, 0x0300085a, 0x00106000, 0x00000003,
        0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x04001858, 0x00107000, 0x00000001, 0x00004444,
        0x04001858, 0x00107000, 0x00000002, 0x00003333, 0x04000858, 0x00107000, 0x00000003, 0x00005555,
        0x040000a2, 0x00107000, 0x00000004, 0x00000004, 0x030000a1, 0x00107000, 0x00000005, 0x04001858,
        0x00107000, 0x00000006, 0x00005555, 0x0300009d, 0x0011e000, 0x00000000, 0x0400009e, 0x0011e000,
        0x00000001, 0x00000010, 0x0300009d, 0x0011e000, 0x00000002, 0x02000068, 0x00000002, 0x0400009b,
        0x00000001, 0x00000001, 0x00000001, 0x0600001c, 0x00100012, 0x00000000, 0x0020800a, 0x00000000,
        0x00000000, 0x06000036, 0x00100022, 0x00000000, 0x0020800a, 0x00000001, 0x00000000, 0x06000036,
        0x00100042, 0x00000000, 0x0020800a, 0x00000002, 0x00000000, 0x05000036, 0x00100082, 0x00000000,
        0x00004001, 0x00000000, 0x070000a6, 0x0011e0f2, 0x00000002, 0x00004001, 0x00000000, 0x00100e46,
        0x00000000, 0x90000048, 0x800000c2, 0x00155543, 0x001000f2, 0x00000000, 0x00004002, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00107e46, 0x00000000, 0x00106000, 0x00000000, 0x00004001,
        0x00000000, 0x0500001c, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x070000a6, 0x0011e0f2,
        0x00000002, 0x00004001, 0x00000010, 0x00100e46, 0x00000000, 0x90000048, 0x800000c2, 0x00155543,
        0x001000f2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00107e46,
        0x00000000, 0x00106000, 0x00000001, 0x00004001, 0x00000000, 0x0500001c, 0x001000f2, 0x00000000,
        0x00100e46, 0x00000000, 0x070000a6, 0x0011e0f2, 0x00000002, 0x00004001, 0x00000020, 0x00100e46,
        0x00000000, 0x90000048, 0x800000c2, 0x00155543, 0x001000f2, 0x00000000, 0x00004002, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00107e46, 0x00000000, 0x00106000, 0x00000002, 0x00004001,
        0x00000000, 0x0500001c, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x070000a6, 0x0011e0f2,
        0x00000002, 0x00004001, 0x00000030, 0x00100e46, 0x00000000, 0x8c00002d, 0x80000042, 0x00155543,
        0x00100012, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00107e46,
        0x00000003, 0x0500001c, 0x00100042, 0x00000000, 0x0010000a, 0x00000000, 0x8b0000a7, 0x80002302,
        0x00199983, 0x00100012, 0x00000001, 0x00004001, 0x00000000, 0x00004001, 0x00000000, 0x00107006,
        0x00000004, 0x0500001c, 0x00100082, 0x00000000, 0x0010000a, 0x00000001, 0x8c00002d, 0x800000c2,
        0x00111103, 0x00100012, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00107e46, 0x00000001, 0x8c00002d, 0x800000c2, 0x000cccc3, 0x00100022, 0x00000000, 0x00004002,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00107e16, 0x00000002, 0x070000a6, 0x0011e0f2,
        0x00000002, 0x00004001, 0x00000040, 0x00100e46, 0x00000000, 0x890000a5, 0x800002c2, 0x00199983,
        0x001000f2, 0x00000000, 0x00004001, 0x00000000, 0x00107e46, 0x00000005, 0x070000a6, 0x0011e0f2,
        0x00000002, 0x00004001, 0x00000050, 0x00100e46, 0x00000000, 0x90000047, 0x800000c2, 0x00155543,
        0x00100012, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00107006,
        0x00000006, 0x00106000, 0x00000003, 0x00004001, 0x3f19999a, 0x0500001c, 0x00100012, 0x00000000,
        0x0010000a, 0x00000000, 0x070000a6, 0x0011e0f2, 0x00000002, 0x00004001, 0x00000060, 0x00100006,
        0x00000000, 0x90000047, 0x800000c2, 0x00155543, 0x00100012, 0x00000000, 0x00004002, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00107006, 0x00000006, 0x00106000, 0x00000003, 0x00004001,
        0x3ecccccd, 0x0500001c, 0x00100012, 0x00000000, 0x0010000a, 0x00000000, 0x070000a6, 0x0011e0f2,
        0x00000002, 0x00004001, 0x00000070, 0x00100006, 0x00000000, 0x890000a5, 0x800002c2, 0x00199983,
        0x00100032, 0x00000000, 0x00004001, 0x00000000, 0x0011e046, 0x00000000, 0x890000a5, 0x800002c2,
        0x00199983, 0x001000c2, 0x00000000, 0x00004001, 0x00000008, 0x0011e406, 0x00000000, 0x070000a6,
        0x0011e0f2, 0x00000002, 0x00004001, 0x00000080, 0x00100e46, 0x00000000, 0x8b0000a7, 0x80008302,
        0x00199983, 0x001000f2, 0x00000000, 0x00004001, 0x00000000, 0x00004001, 0x00000000, 0x0011ee46,
        0x00000001, 0x070000a6, 0x0011e072, 0x00000002, 0x00004001, 0x00000090, 0x00100246, 0x00000000,
        0x0500001c, 0x00100012, 0x00000000, 0x0010003a, 0x00000000, 0x070000a6, 0x0011e0f2, 0x00000002,
        0x00004001, 0x0000009c, 0x00100006, 0x00000000, 0x070000a6, 0x0011e012, 0x00000002, 0x00004001,
        0x000000ac, 0x00004001, 0xdeadbeef, 0x0100003e,
    };
    static const float cb0_data = 10.0f;
    static const UINT cb1_data = 11;
    static const INT cb2_data = -1;
    static const struct vec4 t0_data = {1.0f, 2.0f, 3.0f, 4.0f};
    static const UINT t1_data = 111;
    static const INT t2_data = 222;
    static const float t3_data = 333.3f;
    static const float t4_data = 44.44f;
    static const struct uvec4 t5_data = {50, 51, 52, 53};
    static const struct uvec4 u0_data = {10, 20, 30, 40};
    static const struct data u1_data = {{5, 6, 7}, 10.0f};

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    sampler_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap_desc.NumDescriptors = 2;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cpu_sampler_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cpu_sampler_heap2);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    heap_desc.NumDescriptors = 4;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&sampler_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 30;
    heap_desc.Flags = 0;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cpu_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    /* create samplers */
    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_sampler_heap);
    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ID3D12Device_CreateSampler(context.device, &sampler_desc, cpu_handle);
    sampler_desc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER;
    cpu_handle.ptr += sampler_size;
    ID3D12Device_CreateSampler(context.device, &sampler_desc, cpu_handle);

    /* create CBVs */
    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_heap);
    cb = create_upload_buffer(context.device,
            3 * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, NULL);
    update_buffer_data(cb, 0, sizeof(cb0_data), &cb0_data);
    update_buffer_data(cb, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, sizeof(cb1_data), &cb1_data);
    update_buffer_data(cb, 2 * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, sizeof(cb2_data), &cb2_data);
    cbv_desc.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(cb);
    cbv_desc.SizeInBytes = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    for (i = 0; i < 3; ++i)
    {
        ID3D12Device_CreateConstantBufferView(context.device, &cbv_desc, cpu_handle);
        cbv_desc.BufferLocation += D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        cpu_handle.ptr += descriptor_size;
    }

    /* create SRVs */
    cpu_handle = get_cpu_descriptor_handle(&context, cpu_heap, 10);

    t[0] = create_texture(context.device,
            1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    data.pData = &t0_data;
    data.RowPitch = sizeof(t0_data);
    data.SlicePitch = data.RowPitch;
    upload_texture_data(t[0], &data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, t[0],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    t[1] = create_texture(context.device,
            1, 1, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_STATE_COPY_DEST);
    data.pData = &t1_data;
    data.RowPitch = sizeof(t1_data);
    data.SlicePitch = data.RowPitch;
    upload_texture_data(t[1], &data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, t[1],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    t[2] = create_texture(context.device,
            1, 1, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_STATE_COPY_DEST);
    data.pData = &t2_data;
    data.RowPitch = sizeof(t2_data);
    data.SlicePitch = data.RowPitch;
    upload_texture_data(t[2], &data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, t[2],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    t[3] = create_default_buffer(device, sizeof(t3_data),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(t[3], 0, sizeof(t3_data), &t3_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, t[3],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    t[4] = create_default_buffer(device, sizeof(t4_data),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(t[4], 0, sizeof(t4_data), &t4_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, t[4],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    t[5] = create_default_buffer(device, sizeof(t5_data),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(t[5], 0, sizeof(t5_data), &t5_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, t[5],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    init_depth_stencil(&ds, device, 32, 32, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, NULL);
    t[6] = ds.texture;
    ID3D12Resource_AddRef(t[6]);
    ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, ds.dsv_handle,
            D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, NULL);
    transition_resource_state(command_list, t[6],
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    for (i = 0; i < 3; ++i)
    {
        ID3D12Device_CreateShaderResourceView(device, t[i], NULL, cpu_handle);
        cpu_handle.ptr += descriptor_size;
    }

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.FirstElement = 0;
    srv_desc.Buffer.NumElements = 1;
    ID3D12Device_CreateShaderResourceView(device, t[3], &srv_desc, cpu_handle);
    cpu_handle.ptr += descriptor_size;

    srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_desc.Buffer.StructureByteStride = sizeof(t4_data);
    ID3D12Device_CreateShaderResourceView(device, t[4], &srv_desc, cpu_handle);
    cpu_handle.ptr += descriptor_size;

    srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    srv_desc.Buffer.NumElements = 4;
    srv_desc.Buffer.StructureByteStride = 0;
    srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    ID3D12Device_CreateShaderResourceView(device, t[5], &srv_desc, cpu_handle);
    cpu_handle.ptr += descriptor_size;

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(device, t[6], &srv_desc, cpu_handle);

    /* create UAVs */
    cpu_handle = get_cpu_descriptor_handle(&context, cpu_heap, 20);

    u[0] = create_default_buffer(device, sizeof(u0_data),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(u[0], 0, sizeof(u0_data), &u0_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, u[0],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    u[1] = create_default_buffer(device, sizeof(struct uvec4),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(u[1], 0, sizeof(u1_data), &u1_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, u[0],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    u[2] = create_default_buffer(device, 44 * 4,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = 4;
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(device, u[0], NULL, &uav_desc, cpu_handle);
    cpu_handle.ptr += descriptor_size;

    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.Buffer.NumElements = 1;
    uav_desc.Buffer.StructureByteStride = sizeof(u1_data);
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    ID3D12Device_CreateUnorderedAccessView(device, u[1], NULL, &uav_desc, cpu_handle);
    cpu_handle.ptr += descriptor_size;

    uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.Buffer.NumElements = 44;
    uav_desc.Buffer.StructureByteStride = 0;
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(device, u[2], NULL, &uav_desc, cpu_handle);

    /* root signature */
    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    descriptor_ranges[0].NumDescriptors = 3;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &descriptor_ranges[0];
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    descriptor_ranges[1].NumDescriptors = 4;
    descriptor_ranges[1].BaseShaderRegister = 0;
    descriptor_ranges[1].RegisterSpace = 0;
    descriptor_ranges[1].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &descriptor_ranges[1];
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    descriptor_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[2].NumDescriptors = 7;
    descriptor_ranges[2].BaseShaderRegister = 0;
    descriptor_ranges[2].RegisterSpace = 0;
    descriptor_ranges[2].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[2].DescriptorTable.pDescriptorRanges = &descriptor_ranges[2];
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    descriptor_ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[3].NumDescriptors = 2;
    descriptor_ranges[3].BaseShaderRegister = 0;
    descriptor_ranges[3].RegisterSpace = 0;
    descriptor_ranges[3].OffsetInDescriptorsFromTableStart = 0;
    descriptor_ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[4].NumDescriptors = 1;
    descriptor_ranges[4].BaseShaderRegister = 2;
    descriptor_ranges[4].RegisterSpace = 0;
    descriptor_ranges[4].OffsetInDescriptorsFromTableStart = 2;
    root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[3].DescriptorTable.NumDescriptorRanges = 2;
    root_parameters[3].DescriptorTable.pDescriptorRanges = &descriptor_ranges[3];
    root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 4;
    root_signature_desc.pParameters = root_parameters;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    /* copy descriptors */
    dst_handles[0] = get_cpu_descriptor_handle(&context, heap, 5);
    dst_range_sizes[0] = 2;
    src_handles[0] = get_cpu_descriptor_handle(&context, cpu_heap, 0);
    src_range_sizes[0] = 2;
    /* cb0-cb1 */
    ID3D12Device_CopyDescriptors(device, 1, dst_handles, dst_range_sizes,
            1, src_handles, src_range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dst_handles[0] = get_cpu_descriptor_handle(&context, heap, 7);
    dst_range_sizes[0] = 1;
    src_handles[0] = get_cpu_descriptor_handle(&context, cpu_heap, 2);
    src_range_sizes[0] = 1;
    /* cb2 */
    ID3D12Device_CopyDescriptors(device, 1, dst_handles, dst_range_sizes,
            1, src_handles, src_range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ID3D12Device_CopyDescriptorsSimple(device, 2,
            get_cpu_sampler_handle(&context, cpu_sampler_heap2, 0),
            get_cpu_sampler_handle(&context, cpu_sampler_heap, 0),
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    dst_handles[0] = get_cpu_sampler_handle(&context, sampler_heap, 0);
    dst_range_sizes[0] = 4;
    src_handles[0] = get_cpu_sampler_handle(&context, cpu_sampler_heap2, 0);
    src_range_sizes[0] = 1;
    src_handles[1] = get_cpu_sampler_handle(&context, cpu_sampler_heap2, 0);
    src_range_sizes[1] = 1;
    src_handles[2] = get_cpu_sampler_handle(&context, cpu_sampler_heap2, 0);
    src_range_sizes[2] = 1;
    src_handles[3] = get_cpu_sampler_handle(&context, cpu_sampler_heap2, 1);
    src_range_sizes[3] = 1;
    /* s0-s3 */
    ID3D12Device_CopyDescriptors(device, 1, dst_handles, dst_range_sizes,
            4, src_handles, src_range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    dst_handles[0] = get_cpu_descriptor_handle(&context, heap, 9);
    dst_range_sizes[0] = 4;
    dst_handles[1] = get_cpu_descriptor_handle(&context, heap, 13);
    dst_range_sizes[1] = 3;
    src_handles[0] = get_cpu_descriptor_handle(&context, cpu_heap, 10);
    src_range_sizes[0] = 7;
    /* t0-t6 */
    ID3D12Device_CopyDescriptors(device, 2, dst_handles, dst_range_sizes,
            1, src_handles, src_range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    /* copy 1 uninitialized descriptor (19) */
    dst_handles[0] = get_cpu_descriptor_handle(&context, heap, 19);
    dst_range_sizes[0] = 2;
    dst_handles[1] = get_cpu_descriptor_handle(&context, heap, 21);
    dst_range_sizes[1] = 1;
    src_handles[0] = get_cpu_descriptor_handle(&context, cpu_heap, 19);
    src_range_sizes[0] = 2;
    src_handles[1] = get_cpu_descriptor_handle(&context, cpu_heap, 21);
    src_range_sizes[1] = 1;
    /* u1-u2 */
    ID3D12Device_CopyDescriptors(device, 2, dst_handles, dst_range_sizes,
            2, src_handles, src_range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    /* u2 */
    ID3D12Device_CopyDescriptorsSimple(device, 1,
            get_cpu_descriptor_handle(&context, heap, 22),
            get_cpu_descriptor_handle(&context, cpu_heap, 22),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    heaps[0] = sampler_heap; heaps[1] = heap;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, ARRAY_SIZE(heaps), heaps);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0,
            get_gpu_descriptor_handle(&context, heap, 5));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 1,
            get_gpu_sampler_handle(&context, sampler_heap, 0));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 2,
            get_gpu_descriptor_handle(&context, heap, 9));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 3,
            get_gpu_descriptor_handle(&context, heap, 20));

    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    transition_sub_resource_state(command_list, u[2], 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(u[2], DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    result = get_readback_data(&rb, 0, 0, sizeof(*result));
    ok(result[ 0] == cb0_data, "Got unexpected value %#x.\n", result[0]);
    ok(result[ 1] == cb1_data, "Got unexpected value %#x.\n", result[1]);
    ok(result[ 2] == cb2_data, "Got unexpected value %#x.\n", result[2]);
    ok(result[ 3] == 0, "Got unexpected value %#x.\n", result[3]);
    ok(result[ 4] == t0_data.x, "Got unexpected value %#x.\n", result[4]);
    ok(result[ 5] == t0_data.y, "Got unexpected value %#x.\n", result[5]);
    ok(result[ 6] == t0_data.z, "Got unexpected value %#x.\n", result[6]);
    ok(result[ 7] == t0_data.w, "Got unexpected value %#x.\n", result[7]);
    ok(result[ 8] == t0_data.x, "Got unexpected value %#x.\n", result[8]);
    ok(result[ 9] == t0_data.y, "Got unexpected value %#x.\n", result[9]);
    ok(result[10] == t0_data.z, "Got unexpected value %#x.\n", result[10]);
    ok(result[11] == t0_data.w, "Got unexpected value %#x.\n", result[11]);
    ok(result[12] == t0_data.x, "Got unexpected value %#x.\n", result[12]);
    ok(result[13] == t0_data.y, "Got unexpected value %#x.\n", result[13]);
    ok(result[14] == t0_data.z, "Got unexpected value %#x.\n", result[14]);
    ok(result[15] == t0_data.w, "Got unexpected value %#x.\n", result[15]);
    ok(result[16] == t1_data, "Got unexpected value %#x.\n", result[16]);
    ok(result[17] == t2_data, "Got unexpected value %#x.\n", result[17]);
    ok(result[18] == (unsigned int)t3_data, "Got unexpected value %#x.\n", result[18]);
    ok(result[19] == (unsigned int)t4_data, "Got unexpected value %#x.\n", result[19]);
    ok(result[20] == t5_data.x, "Got unexpected value %#x.\n", result[20]);
    ok(result[21] == t5_data.y, "Got unexpected value %#x.\n", result[21]);
    ok(result[22] == t5_data.z, "Got unexpected value %#x.\n", result[22]);
    ok(result[23] == t5_data.w, "Got unexpected value %#x.\n", result[23]);
    ok(result[24] == 1, "Got unexpected value %#x.\n", result[24]);
    ok(result[25] == 1, "Got unexpected value %#x.\n", result[25]);
    ok(result[26] == 1, "Got unexpected value %#x.\n", result[26]);
    ok(result[27] == 1, "Got unexpected value %#x.\n", result[27]);
    ok(result[28] == 0, "Got unexpected value %#x.\n", result[28]);
    ok(result[29] == 0, "Got unexpected value %#x.\n", result[29]);
    ok(result[30] == 0, "Got unexpected value %#x.\n", result[30]);
    ok(result[31] == 0, "Got unexpected value %#x.\n", result[31]);
    ok(result[32] == u0_data.x, "Got unexpected value %#x.\n", result[32]);
    ok(result[33] == u0_data.y, "Got unexpected value %#x.\n", result[33]);
    ok(result[34] == u0_data.z, "Got unexpected value %#x.\n", result[34]);
    ok(result[35] == u0_data.w, "Got unexpected value %#x.\n", result[35]);
    ok(result[36] == u1_data.u[0], "Got unexpected value %#x.\n", result[36]);
    ok(result[37] == u1_data.u[1], "Got unexpected value %#x.\n", result[37]);
    ok(result[38] == u1_data.u[2], "Got unexpected value %#x.\n", result[38]);
    ok(result[39] == u1_data.f, "Got unexpected value %#x.\n", result[39]);
    ok(result[40] == u1_data.f, "Got unexpected value %#x.\n", result[40]);
    ok(result[41] == u1_data.f, "Got unexpected value %#x.\n", result[41]);
    ok(result[42] == u1_data.f, "Got unexpected value %#x.\n", result[42]);
    ok(result[43] == 0xdeadbeef, "Got unexpected value %#x.\n", result[43]);
    assert(rb.width == 44);
    release_resource_readback(&rb);

    ID3D12DescriptorHeap_Release(cpu_heap);
    ID3D12DescriptorHeap_Release(cpu_sampler_heap);
    ID3D12DescriptorHeap_Release(cpu_sampler_heap2);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12DescriptorHeap_Release(sampler_heap);
    ID3D12Resource_Release(cb);
    for (i = 0; i < ARRAY_SIZE(t); ++i)
        ID3D12Resource_Release(t[i]);
    for (i = 0; i < ARRAY_SIZE(u); ++i)
        ID3D12Resource_Release(u[i]);
    destroy_depth_stencil(&ds);
    destroy_test_context(&context);
}

static void test_descriptors_visibility(void)
{
    ID3D12Resource *vs_raw_buffer, *ps_raw_buffer;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[2];
    D3D12_STATIC_SAMPLER_DESC sampler_desc[2];
    ID3D12Resource *vs_texture, *ps_texture;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[6];
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12Resource *vs_cb, *ps_cb;
    struct test_context_desc desc;
    D3D12_SUBRESOURCE_DATA data;
    struct test_context context;
    ID3D12DescriptorHeap *heap;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    HRESULT hr;

    static const DWORD vs_code[] =
    {
#if 0
        ByteAddressBuffer b;
        Texture2D t;
        SamplerState s;

        float4 cb;

        float4 main(uint id : SV_VertexID) : SV_Position
        {
            float2 coords = float2((id << 1) & 2, id & 2);
            uint i;

            if (cb.x != 4.0 || cb.y != 8.0 || cb.z != 16.0 || cb.w != 32.0)
                return (float4)0;

            for (i = 0; i <= 6; ++i)
            {
                if (b.Load(4 * i) != i)
                    return (float4)0;
            }

            if (any(t.SampleLevel(s, (float2)0, 0) != float4(1.0, 1.0, 0.0, 1.0)))
                return (float4)0;

            return float4(coords * float2(2, -2) + float2(-1, 1), 0, 1);
        }
#endif
        0x43425844, 0x046e4d13, 0xd2103a18, 0x8576703b, 0x6f58933a, 0x00000001, 0x0000043c, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000006, 0x00000001, 0x00000000, 0x00000101, 0x565f5653, 0x65747265, 0x00444978,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000001, 0x00000003,
        0x00000000, 0x0000000f, 0x505f5653, 0x7469736f, 0x006e6f69, 0x58454853, 0x000003a0, 0x00010050,
        0x000000e8, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300005a, 0x00106000,
        0x00000000, 0x030000a1, 0x00107000, 0x00000000, 0x04001858, 0x00107000, 0x00000001, 0x00005555,
        0x04000060, 0x00101012, 0x00000000, 0x00000006, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
        0x02000068, 0x00000002, 0x0b000039, 0x001000f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000,
        0x00004002, 0x40800000, 0x41000000, 0x41800000, 0x42000000, 0x0700003c, 0x00100012, 0x00000000,
        0x0010001a, 0x00000000, 0x0010000a, 0x00000000, 0x0700003c, 0x00100012, 0x00000000, 0x0010002a,
        0x00000000, 0x0010000a, 0x00000000, 0x0700003c, 0x00100012, 0x00000000, 0x0010003a, 0x00000000,
        0x0010000a, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e, 0x01000015, 0x05000036,
        0x00100012, 0x00000000, 0x00004001, 0x00000000, 0x01000030, 0x0700004f, 0x00100022, 0x00000000,
        0x00004001, 0x00000006, 0x0010000a, 0x00000000, 0x03040003, 0x0010001a, 0x00000000, 0x07000029,
        0x00100022, 0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x00000002, 0x890000a5, 0x800002c2,
        0x00199983, 0x00100022, 0x00000000, 0x0010001a, 0x00000000, 0x00107006, 0x00000000, 0x07000027,
        0x00100022, 0x00000000, 0x0010001a, 0x00000000, 0x0010000a, 0x00000000, 0x0304001f, 0x0010001a,
        0x00000000, 0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0100003e, 0x01000015, 0x0700001e, 0x00100012, 0x00000000, 0x0010000a, 0x00000000,
        0x00004001, 0x00000001, 0x01000016, 0x90000048, 0x800000c2, 0x00155543, 0x001000f2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00107e46, 0x00000001, 0x00106000,
        0x00000000, 0x00004001, 0x00000000, 0x0a000039, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000,
        0x00004002, 0x3f800000, 0x3f800000, 0x00000000, 0x3f800000, 0x0700003c, 0x00100032, 0x00000000,
        0x00100ae6, 0x00000000, 0x00100046, 0x00000000, 0x0700003c, 0x00100012, 0x00000000, 0x0010001a,
        0x00000000, 0x0010000a, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2,
        0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e, 0x01000015,
        0x0b00008c, 0x00100012, 0x00000000, 0x00004001, 0x00000001, 0x00004001, 0x00000001, 0x0010100a,
        0x00000000, 0x00004001, 0x00000000, 0x07000001, 0x00100022, 0x00000000, 0x0010100a, 0x00000000,
        0x00004001, 0x00000002, 0x05000056, 0x00100032, 0x00000001, 0x00100046, 0x00000000, 0x0f000032,
        0x00102032, 0x00000000, 0x00100046, 0x00000001, 0x00004002, 0x40000000, 0xc0000000, 0x00000000,
        0x00000000, 0x00004002, 0xbf800000, 0x3f800000, 0x00000000, 0x00000000, 0x08000036, 0x001020c2,
        0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x3f800000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs = {vs_code, sizeof(vs_code)};
    static const DWORD ps_code[] =
    {
#if 0
        ByteAddressBuffer b;
        Texture2D t;
        SamplerState s;

        float4 cb;

        float4 main(float4 position : SV_POSITION) : SV_Target
        {
            if (cb.x != 1.0 || cb.y != 2.0 || cb.z != 3.0 || cb.w != 4.0)
                return float4(1.0, 0.0, 0.0, 1.0);

            if (b.Load(0) != 2 || b.Load(4) != 4 || b.Load(8) != 8)
                return float4(1.0, 0.0, 0.0, 1.0);

            return t.Sample(s, float2(position.x / 32.0, position.y / 32.0));
        }
#endif
        0x43425844, 0x1b1aafc1, 0xeab215f6, 0x77d65b25, 0x03cbe695, 0x00000001, 0x000002dc, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x5449534f, 0x004e4f49,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000240, 0x00000050,
        0x00000090, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300005a, 0x00106000,
        0x00000000, 0x030000a1, 0x00107000, 0x00000000, 0x04001858, 0x00107000, 0x00000001, 0x00005555,
        0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068,
        0x00000001, 0x0b000039, 0x001000f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00004002,
        0x3f800000, 0x40000000, 0x40400000, 0x40800000, 0x0700003c, 0x00100012, 0x00000000, 0x0010001a,
        0x00000000, 0x0010000a, 0x00000000, 0x0700003c, 0x00100012, 0x00000000, 0x0010002a, 0x00000000,
        0x0010000a, 0x00000000, 0x0700003c, 0x00100012, 0x00000000, 0x0010003a, 0x00000000, 0x0010000a,
        0x00000000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000, 0x00004002,
        0x3f800000, 0x00000000, 0x00000000, 0x3f800000, 0x0100003e, 0x01000015, 0x890000a5, 0x800002c2,
        0x00199983, 0x00100072, 0x00000000, 0x00004001, 0x00000000, 0x00107246, 0x00000000, 0x0a000027,
        0x00100072, 0x00000000, 0x00100246, 0x00000000, 0x00004002, 0x00000002, 0x00000004, 0x00000008,
        0x00000000, 0x0700003c, 0x00100012, 0x00000000, 0x0010001a, 0x00000000, 0x0010000a, 0x00000000,
        0x0700003c, 0x00100012, 0x00000000, 0x0010002a, 0x00000000, 0x0010000a, 0x00000000, 0x0304001f,
        0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x00000000,
        0x00000000, 0x3f800000, 0x0100003e, 0x01000015, 0x0a000038, 0x00100032, 0x00000000, 0x00101046,
        0x00000000, 0x00004002, 0x3d000000, 0x3d000000, 0x00000000, 0x00000000, 0x8b000045, 0x800000c2,
        0x00155543, 0x001000f2, 0x00000000, 0x00100046, 0x00000000, 0x00107e46, 0x00000001, 0x00106000,
        0x00000000, 0x05000036, 0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const struct vec4 vs_cb_data = {4.0f, 8.0f, 16.0f, 32.0f};
    static const struct vec4 ps_cb_data = {1.0f, 2.0f, 3.0f, 4.0f};
    static const uint32_t vs_buffer_data[] = {0, 1, 2, 3, 4, 5, 6};
    static const uint32_t ps_buffer_data[] = {2, 4, 8};
    static const float vs_texture_data[] = {1.0f, 1.0f, 0.0f, 1.0f};
    static const float ps_texture_data[] = {0.0f, 1.0f, 0.0f, 1.0f};
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};

    memset(&desc, 0, sizeof(desc));
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    sampler_desc[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc[0].MipLODBias = 0.0f;
    sampler_desc[0].MaxAnisotropy = 0;
    sampler_desc[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler_desc[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler_desc[0].MinLOD = 0.0f;
    sampler_desc[0].MaxLOD = 0.0f;
    sampler_desc[0].ShaderRegister = 0;
    sampler_desc[0].RegisterSpace = 0;
    sampler_desc[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    sampler_desc[1] = sampler_desc[0];
    sampler_desc[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[0].Descriptor.RegisterSpace = 0;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    root_parameters[2].Descriptor.ShaderRegister = 0;
    root_parameters[2].Descriptor.RegisterSpace = 0;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    root_parameters[3].Descriptor.ShaderRegister = 0;
    root_parameters[3].Descriptor.RegisterSpace = 0;
    root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 1;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    root_parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[4].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[4].DescriptorTable.pDescriptorRanges = &descriptor_ranges[0];
    root_parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[1].NumDescriptors = 1;
    descriptor_ranges[1].BaseShaderRegister = 1;
    descriptor_ranges[1].RegisterSpace = 0;
    descriptor_ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    root_parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[5].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[5].DescriptorTable.pDescriptorRanges = &descriptor_ranges[1];
    root_parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 6;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 2;
    root_signature_desc.pStaticSamplers = sampler_desc;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_pipeline_state(device,
            context.root_signature, context.render_target_desc.Format,
            &vs, &ps, NULL);

    vs_cb = create_upload_buffer(device, sizeof(vs_cb_data), &vs_cb_data);
    ps_cb = create_upload_buffer(device, sizeof(ps_cb_data), &ps_cb_data);

    vs_raw_buffer = create_upload_buffer(device, sizeof(vs_buffer_data), vs_buffer_data);
    ps_raw_buffer = create_upload_buffer(device, sizeof(ps_buffer_data), ps_buffer_data);

    vs_texture = create_texture(device,
            1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    data.pData = vs_texture_data;
    data.RowPitch = sizeof(vs_texture_data);
    data.SlicePitch = data.RowPitch;
    upload_texture_data(vs_texture, &data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, vs_texture,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ps_texture = create_texture(device,
            1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    data.pData = ps_texture_data;
    data.RowPitch = sizeof(ps_texture_data);
    data.SlicePitch = data.RowPitch;
    upload_texture_data(ps_texture, &data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, ps_texture,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    ID3D12Device_CreateShaderResourceView(device, vs_texture, NULL,
            get_cpu_descriptor_handle(&context, heap, 0));
    ID3D12Device_CreateShaderResourceView(device, ps_texture, NULL,
            get_cpu_descriptor_handle(&context, heap, 1));

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list,
            0, ID3D12Resource_GetGPUVirtualAddress(vs_cb));
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list,
            1, ID3D12Resource_GetGPUVirtualAddress(ps_cb));
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(command_list,
            2, ID3D12Resource_GetGPUVirtualAddress(vs_raw_buffer));
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(command_list,
            3, ID3D12Resource_GetGPUVirtualAddress(ps_raw_buffer));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list,
            4, get_gpu_descriptor_handle(&context, heap, 0));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list,
            5, get_gpu_descriptor_handle(&context, heap, 1));

    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target, 0, queue, command_list, 0xff00ff00, 0);

    ID3D12Resource_Release(vs_cb);
    ID3D12Resource_Release(ps_cb);
    ID3D12Resource_Release(vs_texture);
    ID3D12Resource_Release(ps_texture);
    ID3D12Resource_Release(vs_raw_buffer);
    ID3D12Resource_Release(ps_raw_buffer);
    ID3D12DescriptorHeap_Release(heap);
    destroy_test_context(&context);
}

#define check_copyable_footprints(a, b, c, d, e, f, g) \
        check_copyable_footprints_(__LINE__, a, b, c, d, e, f, g)
static void check_copyable_footprints_(unsigned int line, const D3D12_RESOURCE_DESC *desc,
        unsigned int sub_resource_idx, unsigned int sub_resource_count,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, const UINT *row_counts,
        const UINT64 *row_sizes, UINT64 *total_size)
{
    unsigned int miplevel, width, height, depth, row_count, row_size, row_pitch;
    UINT64 offset, size, total;
    unsigned int i;

    offset = total = 0;
    for (i = 0; i < sub_resource_count; ++i)
    {
        miplevel = (sub_resource_idx + i) % desc->MipLevels;
        width = align(max(1, desc->Width >> miplevel), format_block_width(desc->Format));
        height = align(max(1, desc->Height >> miplevel), format_block_height(desc->Format));
        depth = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc->DepthOrArraySize : 1;
        depth = max(1, depth >> miplevel);
        row_count = height / format_block_height(desc->Format);
        row_size = (width / format_block_width(desc->Format)) * format_size(desc->Format);
        row_pitch = align(row_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

        if (layouts)
        {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *l = &layouts[i];
            const D3D12_SUBRESOURCE_FOOTPRINT *f = &l->Footprint;

            ok_(line)(l->Offset == offset, "Got offset %"PRIu64", expected %"PRIu64".\n", l->Offset, offset);
            ok_(line)(f->Format == desc->Format, "Got format %#x, expected %#x.\n", f->Format, desc->Format);
            ok_(line)(f->Width == width, "Got width %u, expected %u.\n", f->Width, width);
            ok_(line)(f->Height == height, "Got height %u, expected %u.\n", f->Height, height);
            ok_(line)(f->Depth == depth, "Got depth %u, expected %u.\n", f->Depth, depth);
            ok_(line)(f->RowPitch == row_pitch, "Got row pitch %u, expected %u.\n", f->RowPitch, row_pitch);
        }

        if (row_counts)
            ok_(line)(row_counts[i] == row_count, "Got row count %u, expected %u.\n", row_counts[i], row_count);

        if (row_sizes)
            ok_(line)(row_sizes[i] == row_size, "Got row size %"PRIu64", expected %u.\n", row_sizes[i], row_size);

        size = max(0, row_count - 1) * row_pitch + row_size;
        size = max(0, depth - 1) * align(size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) + size;

        total = offset + size;
        offset = align(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    }

    if (total_size)
        ok_(line)(*total_size == total, "Got total size %"PRIu64", expected %"PRIu64".\n", *total_size, total);
}

static void test_get_copyable_footprints(void)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[10];
    D3D12_RESOURCE_DESC resource_desc;
    UINT64 row_sizes[10], total_size;
    unsigned int sub_resource_count;
    unsigned int i, j, k;
    ID3D12Device *device;
    UINT row_counts[10];
    ULONG refcount;

    static const struct
    {
        D3D12_RESOURCE_DIMENSION dimension;
        unsigned int width;
        unsigned int height;
        unsigned int depth_or_array_size;
        unsigned int miplevel_count;
        bool test_with_compressed;
    }
    resources[] =
    {
        {D3D12_RESOURCE_DIMENSION_BUFFER, 4, 1, 1, 1, false},
        {D3D12_RESOURCE_DIMENSION_TEXTURE1D, 4, 1, 1, 1, false},
        {D3D12_RESOURCE_DIMENSION_TEXTURE1D, 4, 1, 1, 2, false},
        {D3D12_RESOURCE_DIMENSION_TEXTURE1D, 3, 1, 1, 1, false},
        {D3D12_RESOURCE_DIMENSION_TEXTURE1D, 4, 1, 2, 1, false},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, 1, true},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 2, 1, true},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, 2, true},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 3, 1, 1, 2, false},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 3, 2, 1, 2, false},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 3, 1, 1, 1, false},
        {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 3, 2, 1, 1, false},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 4, 4, 1, 1, true},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 4, 4, 2, 1, true},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 4, 4, 2, 2, true},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 8, 8, 8, 4, true},
        {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 3, 2, 2, 2, false},
    };
    static const struct
    {
        DXGI_FORMAT format;
        bool is_compressed;
    }
    formats[] =
    {
        {DXGI_FORMAT_R32G32B32A32_FLOAT, false},
        {DXGI_FORMAT_R32G32B32A32_UINT, false},
        {DXGI_FORMAT_R32_UINT, false},
        {DXGI_FORMAT_R8G8B8A8_UNORM, false},
        {DXGI_FORMAT_BC1_UNORM, true},
        {DXGI_FORMAT_BC2_UNORM, true},
        {DXGI_FORMAT_BC3_UNORM, true},
        {DXGI_FORMAT_BC4_UNORM, true},
        {DXGI_FORMAT_BC5_UNORM, true},
        {DXGI_FORMAT_BC6H_UF16, true},
        {DXGI_FORMAT_BC6H_SF16, true},
        {DXGI_FORMAT_BC7_UNORM, true},
    };
    static const struct
    {
        D3D12_RESOURCE_DESC resource_desc;
        unsigned int sub_resource_idx;
        unsigned int sub_resource_count;
    }
    invalid_descs[] =
    {
        {
            {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 3, 2, 1, 1, DXGI_FORMAT_R32_UINT,
                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE}, 0, 1,
        },
        {
            {D3D12_RESOURCE_DIMENSION_TEXTURE1D, 0, 4, 2, 1, 1, DXGI_FORMAT_R32_UINT,
                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE}, 0, 1,
        },
        {
            {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R32_UINT,
                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE}, 0, 2,
        },
        {
            {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 3, 1, 1, 2, DXGI_FORMAT_BC1_UNORM,
                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE}, 0, 2,
        },
        {
            {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 3, 1, 1, 1, DXGI_FORMAT_BC1_UNORM,
                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE}, 0, 1,
        },
        {
            {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 3, 1, 1, 2, DXGI_FORMAT_BC7_UNORM,
                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE}, 0, 2,
        },
        {
            {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 3, 1, 1, 1, DXGI_FORMAT_BC7_UNORM,
                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE}, 0, 1,
        },
        {
            {D3D12_RESOURCE_DIMENSION_TEXTURE3D, 3, 2, 2, 2, 2, DXGI_FORMAT_BC1_UNORM,
                {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE}, 0, 1,
        },
    };

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    /* TODO: test base offset */
    for (i = 0; i < ARRAY_SIZE(resources); ++i)
    {
        const bool is_buffer = resources[i].dimension == D3D12_RESOURCE_DIMENSION_BUFFER;

        resource_desc.Dimension = resources[i].dimension;
        resource_desc.Alignment = 0;
        resource_desc.Width = resources[i].width;
        resource_desc.Height = resources[i].height;
        resource_desc.DepthOrArraySize = resources[i].depth_or_array_size;
        resource_desc.MipLevels = resources[i].miplevel_count;

        for (j = 0; j < ARRAY_SIZE(formats); ++j)
        {
            if (formats[j].is_compressed && !resources[i].test_with_compressed)
                continue;
            if (is_buffer && j > 0)
                continue;

            if (is_buffer)
                resource_desc.Format = DXGI_FORMAT_UNKNOWN;
            else
                resource_desc.Format = formats[j].format;

            resource_desc.SampleDesc.Count = 1;
            resource_desc.SampleDesc.Quality = 0;
            resource_desc.Layout = is_buffer ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
            resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            sub_resource_count = resource_desc.MipLevels;
            if (resources[i].dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                sub_resource_count *= resource_desc.DepthOrArraySize;
            assert(sub_resource_count <= ARRAY_SIZE(layouts));

            memset(layouts, 0, sizeof(layouts));
            memset(row_counts, 0, sizeof(row_counts));
            memset(row_sizes, 0, sizeof(row_sizes));
            total_size = 0;
            ID3D12Device_GetCopyableFootprints(device, &resource_desc, 0, sub_resource_count, 0,
                    layouts, row_counts, row_sizes, &total_size);
            check_copyable_footprints(&resource_desc, 0, sub_resource_count,
                    layouts, row_counts, row_sizes, &total_size);

            memset(layouts, 0, sizeof(layouts));
            ID3D12Device_GetCopyableFootprints(device, &resource_desc, 0, sub_resource_count, 0,
                    layouts, NULL, NULL, NULL);
            check_copyable_footprints(&resource_desc, 0, sub_resource_count,
                    layouts, NULL, NULL, NULL);
            memset(row_counts, 0, sizeof(row_counts));
            ID3D12Device_GetCopyableFootprints(device, &resource_desc, 0, sub_resource_count, 0,
                    NULL, row_counts, NULL, NULL);
            check_copyable_footprints(&resource_desc, 0, sub_resource_count,
                    NULL, row_counts, NULL, NULL);
            memset(row_sizes, 0, sizeof(row_sizes));
            ID3D12Device_GetCopyableFootprints(device, &resource_desc, 0, sub_resource_count, 0,
                    NULL, NULL, row_sizes, NULL);
            check_copyable_footprints(&resource_desc, 0, sub_resource_count,
                    NULL, NULL, row_sizes, NULL);
            total_size = 0;
            ID3D12Device_GetCopyableFootprints(device, &resource_desc, 0, sub_resource_count, 0,
                    NULL, NULL, NULL, &total_size);
            check_copyable_footprints(&resource_desc, 0, sub_resource_count,
                    NULL, NULL, NULL, &total_size);

            for (k = 0; k < sub_resource_count; ++k)
            {
                memset(layouts, 0, sizeof(layouts));
                memset(row_counts, 0, sizeof(row_counts));
                memset(row_sizes, 0, sizeof(row_sizes));
                total_size = 0;
                ID3D12Device_GetCopyableFootprints(device, &resource_desc, k, 1, 0,
                        layouts, row_counts, row_sizes, &total_size);
                check_copyable_footprints(&resource_desc, k, 1,
                        layouts, row_counts, row_sizes, &total_size);
            }
        }
    }

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 512;
    resource_desc.Height = 512;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource_desc.SampleDesc.Count = 4;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    memset(layouts, 0, sizeof(layouts));
    memset(row_counts, 0, sizeof(row_counts));
    memset(row_sizes, 0, sizeof(row_sizes));
    total_size = 0;
    ID3D12Device_GetCopyableFootprints(device, &resource_desc, 0, 1, 0,
            layouts, row_counts, row_sizes, &total_size);
    check_copyable_footprints(&resource_desc, 0, 1,
            layouts, row_counts, row_sizes, &total_size);

    for (i = 0; i < ARRAY_SIZE(invalid_descs); ++i)
    {
        resource_desc = invalid_descs[i].resource_desc;

        memset(layouts, 0, sizeof(layouts));
        memset(row_counts, 0, sizeof(row_counts));
        memset(row_sizes, 0, sizeof(row_sizes));
        total_size = 0;
        ID3D12Device_GetCopyableFootprints(device, &resource_desc,
                invalid_descs[i].sub_resource_idx, invalid_descs[i].sub_resource_count, 0,
                layouts, row_counts, row_sizes, &total_size);

        for (j = 0; j < invalid_descs[i].sub_resource_count; ++j)
        {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *l = &layouts[j];

            ok(l->Offset == ~(UINT64)0, "Got offset %"PRIu64".\n", l->Offset);
            ok(l->Footprint.Format == ~(DXGI_FORMAT)0, "Got format %#x.\n", l->Footprint.Format);
            ok(l->Footprint.Width == ~0u, "Got width %u.\n", l->Footprint.Width);
            ok(l->Footprint.Height == ~0u, "Got height %u.\n", l->Footprint.Height);
            ok(l->Footprint.Depth == ~0u, "Got depth %u.\n", l->Footprint.Depth);
            ok(l->Footprint.RowPitch == ~0u, "Got row pitch %u.\n", l->Footprint.RowPitch);

            ok(row_counts[j] == ~0u, "Got row count %u.\n", row_counts[j]);
            ok(row_sizes[j] == ~(UINT64)0, "Got row size %"PRIu64".\n", row_sizes[j]);
        }

        ok(total_size == ~(UINT64)0, "Got total size %"PRIu64".\n", total_size);
    }

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

#define check_depth_stencil_sampling(a, b, c, d, e, f, g) \
        check_depth_stencil_sampling_(__LINE__, a, b, c, d, e, f, g)
static void check_depth_stencil_sampling_(unsigned int line, struct test_context *context,
        ID3D12PipelineState *pso, ID3D12Resource *cb, ID3D12Resource *texture,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle, ID3D12DescriptorHeap *srv_heap,
        float expected_value)
{
    static const float black[] = {0.0f, 0.0f, 0.0f, 0.0f};
    ID3D12GraphicsCommandList *command_list;
    ID3D12CommandQueue *queue;
    HRESULT hr;

    command_list = context->list;
    queue = context->queue;

    transition_sub_resource_state(command_list, texture, 0,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context->rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context->rtv, FALSE, NULL);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, pso);

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context->root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &srv_heap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0,
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap));
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context->viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context->scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_sub_resource_state(command_list, context->render_target, 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_float_(line, context->render_target, 0, queue, command_list, expected_value, 2);

    reset_command_list(command_list, context->allocator);
    transition_sub_resource_state(command_list, context->render_target, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition_sub_resource_state(command_list, texture, 0,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok_(line)(SUCCEEDED(hr), "Failed to close command list, hr %#x.\n", hr);
    exec_command_list(queue, command_list);
    wait_queue_idle(context->device, queue);
}

static void test_depth_stencil_sampling(void)
{
    ID3D12PipelineState *pso_compare, *pso_depth, *pso_stencil, *pso_depth_stencil;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle, srv_cpu_handle;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_STATIC_SAMPLER_DESC sampler_desc[2];
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_range;
    D3D12_ROOT_PARAMETER root_parameters[2];
    ID3D12GraphicsCommandList *command_list;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    struct depth_stencil_resource ds;
    ID3D12DescriptorHeap *srv_heap;
    struct test_context_desc desc;
    ID3D12Resource *cb, *texture;
    unsigned int descriptor_size;
    struct test_context context;
    struct vec4 ps_constant;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    static const DWORD ps_compare_code[] =
    {
#if 0
        Texture2D t;
        SamplerComparisonState s : register(s1);

        float ref;

        float4 main(float4 position : SV_Position) : SV_Target
        {
            return t.SampleCmp(s, float2(position.x / 640.0f, position.y / 480.0f), ref);
        }
#endif
        0x43425844, 0xbea899fb, 0xcbeaa744, 0xbad6daa0, 0xd4363d30, 0x00000001, 0x00000164, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x000000c8, 0x00000040,
        0x00000032, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300085a, 0x00106000, 0x00000001,
        0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x04002064, 0x00101032, 0x00000000, 0x00000001,
        0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x0a000038, 0x00100032, 0x00000000,
        0x00101046, 0x00000000, 0x00004002, 0x3acccccd, 0x3b088889, 0x00000000, 0x00000000, 0x0c000046,
        0x00100012, 0x00000000, 0x00100046, 0x00000000, 0x00107006, 0x00000000, 0x00106000, 0x00000001,
        0x0020800a, 0x00000000, 0x00000000, 0x05000036, 0x001020f2, 0x00000000, 0x00100006, 0x00000000,
        0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_compare = {ps_compare_code, sizeof(ps_compare_code)};
    static const DWORD ps_sample_code[] =
    {
#if 0
        Texture2D t;
        SamplerState s;

        float4 main(float4 position : SV_Position) : SV_Target
        {
            return t.Sample(s, float2(position.x / 640.0f, position.y / 480.0f));
        }
#endif
        0x43425844, 0x7472c092, 0x5548f00e, 0xf4e007f1, 0x5970429c, 0x00000001, 0x00000134, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x00000098, 0x00000040,
        0x00000026, 0x0300005a, 0x00106000, 0x00000000, 0x04001858, 0x00107000, 0x00000000, 0x00005555,
        0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068,
        0x00000001, 0x0a000038, 0x00100032, 0x00000000, 0x00101046, 0x00000000, 0x00004002, 0x3acccccd,
        0x3b088889, 0x00000000, 0x00000000, 0x09000045, 0x001020f2, 0x00000000, 0x00100046, 0x00000000,
        0x00107e46, 0x00000000, 0x00106000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_sample = {ps_sample_code, sizeof(ps_sample_code)};
    static const DWORD ps_stencil_code[] =
    {
#if 0
        Texture2D<uint4> t : register(t1);

        float4 main(float4 position : SV_Position) : SV_Target
        {
            float2 s;
            t.GetDimensions(s.x, s.y);
            return t.Load(int3(float3(s.x * position.x / 640.0f, s.y * position.y / 480.0f, 0))).y;
        }
#endif
        0x43425844, 0x78574912, 0x1b7763f5, 0x0124de83, 0x39954d6c, 0x00000001, 0x000001a0, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x00000104, 0x00000040,
        0x00000041, 0x04001858, 0x00107000, 0x00000001, 0x00004444, 0x04002064, 0x00101032, 0x00000000,
        0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x0700003d, 0x001000f2,
        0x00000000, 0x00004001, 0x00000000, 0x00107e46, 0x00000001, 0x07000038, 0x00100032, 0x00000000,
        0x00100046, 0x00000000, 0x00101046, 0x00000000, 0x0a000038, 0x00100032, 0x00000000, 0x00100046,
        0x00000000, 0x00004002, 0x3acccccd, 0x3b088889, 0x00000000, 0x00000000, 0x0500001b, 0x00100032,
        0x00000000, 0x00100046, 0x00000000, 0x08000036, 0x001000c2, 0x00000000, 0x00004002, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x0700002d, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000,
        0x00107e46, 0x00000001, 0x05000056, 0x001020f2, 0x00000000, 0x00100556, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_stencil = {ps_stencil_code, sizeof(ps_stencil_code)};
    static const DWORD ps_depth_stencil_code[] =
    {
#if 0
        SamplerState samp;
        Texture2D depth_tex;
        Texture2D<uint4> stencil_tex;

        float main(float4 position: SV_Position) : SV_Target
        {
            float2 s, p;
            float depth, stencil;
            depth_tex.GetDimensions(s.x, s.y);
            p = float2(s.x * position.x / 640.0f, s.y * position.y / 480.0f);
            depth = depth_tex.Sample(samp, p).r;
            stencil = stencil_tex.Load(int3(float3(p.x, p.y, 0))).y;
            return depth + stencil;
        }
#endif
        0x43425844, 0x348f8377, 0x977d1ee0, 0x8cca4f35, 0xff5c5afc, 0x00000001, 0x000001fc, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x00000e01, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x00000160, 0x00000040,
        0x00000058, 0x0300005a, 0x00106000, 0x00000000, 0x04001858, 0x00107000, 0x00000000, 0x00005555,
        0x04001858, 0x00107000, 0x00000001, 0x00004444, 0x04002064, 0x00101032, 0x00000000, 0x00000001,
        0x03000065, 0x00102012, 0x00000000, 0x02000068, 0x00000002, 0x0700003d, 0x001000f2, 0x00000000,
        0x00004001, 0x00000000, 0x00107e46, 0x00000000, 0x07000038, 0x00100032, 0x00000000, 0x00100046,
        0x00000000, 0x00101046, 0x00000000, 0x0a000038, 0x00100032, 0x00000000, 0x00100046, 0x00000000,
        0x00004002, 0x3acccccd, 0x3b088889, 0x00000000, 0x00000000, 0x0500001b, 0x00100032, 0x00000001,
        0x00100046, 0x00000000, 0x09000045, 0x001000f2, 0x00000000, 0x00100046, 0x00000000, 0x00107e46,
        0x00000000, 0x00106000, 0x00000000, 0x08000036, 0x001000c2, 0x00000001, 0x00004002, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x0700002d, 0x001000f2, 0x00000001, 0x00100e46, 0x00000001,
        0x00107e46, 0x00000001, 0x05000056, 0x00100022, 0x00000000, 0x0010001a, 0x00000001, 0x07000000,
        0x00102012, 0x00000000, 0x0010001a, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_depth_stencil = {ps_depth_stencil_code, sizeof(ps_depth_stencil_code)};
    static const struct test
    {
        DXGI_FORMAT typeless_format;
        DXGI_FORMAT dsv_format;
        DXGI_FORMAT depth_view_format;
        DXGI_FORMAT stencil_view_format;
    }
    tests[] =
    {
        {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT},
        {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT,
                DXGI_FORMAT_R32_FLOAT},
        {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT,
                DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT},
        {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM,
                DXGI_FORMAT_R16_UNORM},
    };

    memset(&desc, 0, sizeof(desc));
    desc.rt_width = 640;
    desc.rt_height = 480;
    desc.rt_format = DXGI_FORMAT_R32_FLOAT;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;

    sampler_desc[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc[0].MipLODBias = 0.0f;
    sampler_desc[0].MaxAnisotropy = 0;
    sampler_desc[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler_desc[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler_desc[0].MinLOD = 0.0f;
    sampler_desc[0].MaxLOD = 0.0f;
    sampler_desc[0].ShaderRegister = 0;
    sampler_desc[0].RegisterSpace = 0;
    sampler_desc[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    sampler_desc[1] = sampler_desc[0];
    sampler_desc[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    sampler_desc[1].ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER;
    sampler_desc[1].ShaderRegister = 1;

    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range.NumDescriptors = 2;
    descriptor_range.BaseShaderRegister = 0;
    descriptor_range.RegisterSpace = 0;
    descriptor_range.OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &descriptor_range;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 2;
    root_signature_desc.pStaticSamplers = sampler_desc;
    hr = create_root_signature(context.device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    pso_compare = create_pipeline_state(device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps_compare, NULL);
    pso_depth = create_pipeline_state(device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps_sample, NULL);
    pso_stencil = create_pipeline_state(device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps_stencil, NULL);
    pso_depth_stencil = create_pipeline_state(device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps_depth_stencil, NULL);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&srv_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    memset(&ps_constant, 0, sizeof(ps_constant));
    cb = create_upload_buffer(device, sizeof(ps_constant), &ps_constant);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Failed to close command list, hr %#x.\n", hr);

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        reset_command_list(command_list, context.allocator);

        init_depth_stencil(&ds, device, context.render_target_desc.Width,
                context.render_target_desc.Height, tests[i].typeless_format,
                tests[i].dsv_format, NULL);
        texture = ds.texture;
        dsv_handle = ds.dsv_handle;

        srv_cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap);

        memset(&srv_desc, 0, sizeof(srv_desc));
        srv_desc.Format = tests[i].depth_view_format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        ID3D12Device_CreateShaderResourceView(device, texture, &srv_desc, srv_cpu_handle);
        srv_cpu_handle.ptr += descriptor_size;
        ID3D12Device_CreateShaderResourceView(device, NULL, &srv_desc, srv_cpu_handle);

        ps_constant.x = 0.5f;
        update_buffer_data(cb, 0, sizeof(ps_constant), &ps_constant);

        /* pso_compare */
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
        check_depth_stencil_sampling(&context, pso_compare, cb, texture, dsv_handle, srv_heap, 0.0f);

        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, NULL);
        check_depth_stencil_sampling(&context, pso_compare, cb, texture, dsv_handle, srv_heap, 1.0f);

        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, NULL);
        check_depth_stencil_sampling(&context, pso_compare, cb, texture, dsv_handle, srv_heap, 0.0f);

        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH, 0.6f, 0, 0, NULL);
        check_depth_stencil_sampling(&context, pso_compare, cb, texture, dsv_handle, srv_heap, 0.0f);

        ps_constant.x = 0.7f;
        update_buffer_data(cb, 0, sizeof(ps_constant), &ps_constant);

        reset_command_list(command_list, context.allocator);
        check_depth_stencil_sampling(&context, pso_compare, cb, texture, dsv_handle, srv_heap, 1.0f);

        /* pso_depth */
        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
        check_depth_stencil_sampling(&context, pso_depth, cb, texture, dsv_handle, srv_heap, 1.0f);

        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH, 0.2f, 0, 0, NULL);
        check_depth_stencil_sampling(&context, pso_depth, cb, texture, dsv_handle, srv_heap, 0.2f);

        if (!tests[i].stencil_view_format)
        {
            destroy_depth_stencil(&ds);
            continue;
        }
        srv_desc.Format = tests[i].stencil_view_format;
        srv_desc.Texture2D.PlaneSlice = 1;
        ID3D12Device_CreateShaderResourceView(device, texture, &srv_desc, srv_cpu_handle);

        /* pso_stencil */
        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, NULL);
        check_depth_stencil_sampling(&context, pso_stencil, cb, texture, dsv_handle, srv_heap, 0.0f);

        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_STENCIL, 0.0f, 100, 0, NULL);
        check_depth_stencil_sampling(&context, pso_stencil, cb, texture, dsv_handle, srv_heap, 100.0f);

        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_STENCIL, 0.0f, 255, 0, NULL);
        check_depth_stencil_sampling(&context, pso_stencil, cb, texture, dsv_handle, srv_heap, 255.0f);

        /* pso_depth_stencil */
        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.3f, 3, 0, NULL);
        check_depth_stencil_sampling(&context, pso_depth_stencil, cb, texture, dsv_handle, srv_heap, 3.3f);

        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 3, 0, NULL);
        check_depth_stencil_sampling(&context, pso_depth_stencil, cb, texture, dsv_handle, srv_heap, 4.0f);

        reset_command_list(command_list, context.allocator);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, NULL);
        check_depth_stencil_sampling(&context, pso_depth_stencil, cb, texture, dsv_handle, srv_heap, 0.0f);

        destroy_depth_stencil(&ds);
    }

    ID3D12Resource_Release(cb);
    ID3D12DescriptorHeap_Release(srv_heap);
    ID3D12PipelineState_Release(pso_compare);
    ID3D12PipelineState_Release(pso_depth);
    ID3D12PipelineState_Release(pso_stencil);
    ID3D12PipelineState_Release(pso_depth_stencil);
    destroy_test_context(&context);
}

static void test_depth_load(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[2];
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D12_ROOT_PARAMETER root_parameters[1];
    ID3D12GraphicsCommandList *command_list;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12PipelineState *pipeline_state;
    struct depth_stencil_resource ds;
    struct test_context_desc desc;
    struct test_context context;
    ID3D12DescriptorHeap *heap;
    ID3D12CommandQueue *queue;
    ID3D12Resource *texture;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        Texture2D<float> t;
        RWTexture2D<float> u;

        [numthreads(1, 1, 1)]
        void main(uint2 id : SV_GroupID)
        {
            u[id] = t[id];
        }
#endif
        0x43425844, 0x6ddce3d0, 0x24b47ad3, 0x7f6772d2, 0x6a644890, 0x00000001, 0x00000110, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000000bc, 0x00050050, 0x0000002f, 0x0100086a,
        0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x0400189c, 0x0011e000, 0x00000000, 0x00005555,
        0x0200005f, 0x00021032, 0x02000068, 0x00000001, 0x0400009b, 0x00000001, 0x00000001, 0x00000001,
        0x04000036, 0x00100032, 0x00000000, 0x00021046, 0x08000036, 0x001000c2, 0x00000000, 0x00004002,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x8900002d, 0x800000c2, 0x00155543, 0x00100012,
        0x00000000, 0x00100e46, 0x00000000, 0x00107e46, 0x00000000, 0x060000a4, 0x0011e0f2, 0x00000000,
        0x00021546, 0x00100006, 0x00000000, 0x0100003e,
    };
    static const DWORD ps_code[] =
    {
#if 0
        Texture2D<float> t;

        float main(float4 position : SV_Position) : SV_Target
        {
            return t[int2(position.x, position.y)];
        }
#endif
        0x43425844, 0xe3ea64c3, 0xd67e08d8, 0xfd408a6d, 0x2eaf0b44, 0x00000001, 0x0000023c, 0x00000005,
        0x00000034, 0x000000ac, 0x000000e0, 0x00000114, 0x000001c0, 0x46454452, 0x00000070, 0x00000000,
        0x00000000, 0x00000001, 0x0000001c, 0xffff0400, 0x00000100, 0x0000003e, 0x0000003c, 0x00000002,
        0x00000005, 0x00000004, 0xffffffff, 0x00000000, 0x00000001, 0x00000000, 0x694d0074, 0x736f7263,
        0x2074666f, 0x20295228, 0x4c534c48, 0x61685320, 0x20726564, 0x706d6f43, 0x72656c69, 0x322e3920,
        0x35392e39, 0x31332e32, 0xab003131, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x00000e01, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x000000a4, 0x00000040,
        0x00000029, 0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x04002064, 0x00101032, 0x00000000,
        0x00000001, 0x03000065, 0x00102012, 0x00000000, 0x02000068, 0x00000001, 0x0500001b, 0x00100032,
        0x00000000, 0x00101046, 0x00000000, 0x08000036, 0x001000c2, 0x00000000, 0x00004002, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x0700002d, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000,
        0x00107e46, 0x00000000, 0x05000036, 0x00102012, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
        0x54415453, 0x00000074, 0x00000005, 0x00000001, 0x00000000, 0x00000002, 0x00000000, 0x00000000,
        0x00000000, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000002, 0x00000000, 0x00000001,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const float tests[] = {0.00f, 0.25f, 0.75f, 1.00f};

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32_FLOAT;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[1].NumDescriptors = 1;
    descriptor_ranges[1].BaseShaderRegister = 0;
    descriptor_ranges[1].RegisterSpace = 0;
    descriptor_ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 2;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    pipeline_state = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, context.render_target_desc.Format, NULL, &ps, NULL);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    init_depth_stencil(&ds, device, context.render_target_desc.Width,
            context.render_target_desc.Height, DXGI_FORMAT_R32_TYPELESS,
            DXGI_FORMAT_D32_FLOAT, NULL);
    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(device, ds.texture, &srv_desc,
            get_cpu_descriptor_handle(&context, heap, 0));

    texture = create_default_texture(device, 32, 32, DXGI_FORMAT_R16_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Device_CreateUnorderedAccessView(device, texture, NULL, NULL,
            get_cpu_descriptor_handle(&context, heap, 1));

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, ds.dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH, tests[i], 0, 0, NULL);
        transition_sub_resource_state(command_list, ds.texture, 0,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);

        ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0,
                ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap));
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
        ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0,
                ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap));
        ID3D12GraphicsCommandList_Dispatch(command_list, 32, 32, 1);

        transition_sub_resource_state(command_list, context.render_target, 0,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check_sub_resource_float(context.render_target, 0, queue, command_list, tests[i], 2);

        reset_command_list(command_list, context.allocator);
        transition_sub_resource_state(command_list, texture, 0,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check_sub_resource_uint16(texture, 0, queue, command_list, tests[i] * UINT16_MAX, 2);

        reset_command_list(command_list, context.allocator);
        transition_sub_resource_state(command_list, context.render_target, 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition_sub_resource_state(command_list, texture, 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition_sub_resource_state(command_list, ds.texture, 0,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    destroy_depth_stencil(&ds);
    ID3D12Resource_Release(texture);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12PipelineState_Release(pipeline_state);
    destroy_test_context(&context);
}

static void test_typed_buffer_uav(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor_handle;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[1];
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    ID3D12Device *device;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        RWBuffer<float> buffer;

        [numthreads(32, 1, 1)]
        void main(uint3 group_id : SV_groupID, uint group_index : SV_GroupIndex)
        {
            uint global_index = 32 * group_id.x + group_index;
            buffer[global_index] = 0.5f;
        }
#endif
        0x43425844, 0xcc416762, 0xde23c7b7, 0x4012ae1f, 0xaed30ba4, 0x00000001, 0x000000e0, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x0000008c, 0x00050050, 0x00000023, 0x0100086a,
        0x0400089c, 0x0011e000, 0x00000000, 0x00005555, 0x0200005f, 0x00024000, 0x0200005f, 0x00021012,
        0x02000068, 0x00000001, 0x0400009b, 0x00000020, 0x00000001, 0x00000001, 0x07000023, 0x00100012,
        0x00000000, 0x0002100a, 0x00004001, 0x00000020, 0x0002400a, 0x0a0000a4, 0x0011e0f2, 0x00000000,
        0x00100006, 0x00000000, 0x00004002, 0x3f000000, 0x3f000000, 0x3f000000, 0x3f000000, 0x0100003e,
    };

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    pipeline_state = create_compute_pipeline_state(device, root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap);
    gpu_descriptor_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap);

    resource = create_default_buffer(device, 64 * sizeof(float),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = 64;
    uav_desc.Buffer.StructureByteStride = 0;
    uav_desc.Buffer.CounterOffsetInBytes = 0;
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    ID3D12Device_CreateUnorderedAccessView(device, resource, NULL, &uav_desc, cpu_descriptor_handle);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_Dispatch(command_list, 2, 1, 1);

    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(resource, uav_desc.Format, &rb, queue, command_list);
    check_readback_data_float(&rb, NULL, 0.5f, 0);
    release_resource_readback(&rb);

    ID3D12Resource_Release(resource);
    ID3D12RootSignature_Release(root_signature);
    ID3D12PipelineState_Release(pipeline_state);
    ID3D12DescriptorHeap_Release(descriptor_heap);
    destroy_test_context(&context);
}

static void test_typed_uav_store(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[2];
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_float_code[] =
    {
#if 0
        RWTexture2D<float> u;

        float f;

        [numthreads(1, 1, 1)]
        void main(uint2 id : SV_GroupID)
        {
            u[id] = f;
        }
#endif
        0x43425844, 0xc3add41b, 0x67df51b1, 0x2b887930, 0xcb1ee991, 0x00000001, 0x000000b8, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000064, 0x00050050, 0x00000019, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0400189c, 0x0011e000, 0x00000000, 0x00005555,
        0x0200005f, 0x00021032, 0x0400009b, 0x00000001, 0x00000001, 0x00000001, 0x070000a4, 0x0011e0f2,
        0x00000000, 0x00021546, 0x00208006, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const struct
    {
        DXGI_FORMAT format;
        float constant;
        union
        {
            float f;
            uint16_t u16;
        } result;
    }
    tests[] =
    {
        {DXGI_FORMAT_R16_FLOAT, 1.0f, {.u16 = 0x3c00}},
        {DXGI_FORMAT_R16_FLOAT, 0.5f, {.u16 = 0x3800}},

        {DXGI_FORMAT_R16_UNORM, 0.5f, {.u16 = 32768}},

        {DXGI_FORMAT_R32_FLOAT, 0.0f, {.f = 0.0f}},
        {DXGI_FORMAT_R32_FLOAT, 0.5f, {.f = 0.5f}},
        {DXGI_FORMAT_R32_FLOAT, 1.0f, {.f = 1.0f}},
    };

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 0;
    root_parameters[1].Constants.RegisterSpace = 0;
    root_parameters[1].Constants.Num32BitValues = 1;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_float_code, sizeof(cs_float_code)));

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        resource = create_default_texture(device, 32, 32, tests[i].format,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Device_CreateUnorderedAccessView(device, resource, NULL, NULL,
                ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap));

        ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0,
                ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap));
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(command_list, 1,
                1, &tests[i].constant, 0);
        ID3D12GraphicsCommandList_Dispatch(command_list, 32, 32, 1);

        transition_sub_resource_state(command_list, resource, 0,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        switch (tests[i].format)
        {
            default:
                trace("Unhandled format %#x.\n", tests[i].format);
            case DXGI_FORMAT_R32_FLOAT:
                check_sub_resource_float(resource, 0, queue, command_list, tests[i].result.f, 2);
                break;
            case DXGI_FORMAT_R16_FLOAT:
            case DXGI_FORMAT_R16_UNORM:
                check_sub_resource_uint16(resource, 0, queue, command_list, tests[i].result.u16, 2);
                break;
        }

        ID3D12Resource_Release(resource);

        reset_command_list(command_list, context.allocator);
    }

    ID3D12DescriptorHeap_Release(descriptor_heap);
    destroy_test_context(&context);
}

static void test_compute_shader_registers(void)
{
    struct data
    {
        unsigned int group_id[3];
        unsigned int group_index;
        unsigned int dispatch_id[3];
        unsigned int thread_id[3];
    };

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor_handle;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[2];
    unsigned int i, x, y, group_x, group_y;
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    const struct data *data;
    struct uvec4 dimensions;
    ID3D12Device *device;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        struct data
        {
            uint3 group_id;
            uint group_index;
            uint3 dispatch_id;
            uint3 group_thread_id;
        };

        RWStructuredBuffer<data> u;

        uint2 dim;

        [numthreads(3, 2, 1)]
        void main(uint3 group_id : SV_GroupID,
                uint group_index : SV_GroupIndex,
                uint3 dispatch_id : SV_DispatchThreadID,
                uint3 group_thread_id : SV_GroupThreadID)
        {
            uint i = dispatch_id.x + dispatch_id.y * 3 * dim.x;
            u[i].group_id = group_id;
            u[i].group_index = group_index;
            u[i].dispatch_id = dispatch_id;
            u[i].group_thread_id = group_thread_id;
        }
#endif
        0x43425844, 0xf0bce218, 0xfc1e8267, 0xe6d57544, 0x342df592, 0x00000001, 0x000001a4, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000150, 0x00050050, 0x00000054, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0400009e, 0x0011e000, 0x00000000, 0x00000028,
        0x0200005f, 0x00024000, 0x0200005f, 0x00021072, 0x0200005f, 0x00022072, 0x0200005f, 0x00020072,
        0x02000068, 0x00000002, 0x0400009b, 0x00000003, 0x00000002, 0x00000001, 0x04000036, 0x00100072,
        0x00000000, 0x00021246, 0x04000036, 0x00100082, 0x00000000, 0x0002400a, 0x08000026, 0x0000d000,
        0x00100012, 0x00000001, 0x0002001a, 0x0020800a, 0x00000000, 0x00000000, 0x08000023, 0x00100012,
        0x00000001, 0x0010000a, 0x00000001, 0x00004001, 0x00000003, 0x0002000a, 0x090000a8, 0x0011e0f2,
        0x00000000, 0x0010000a, 0x00000001, 0x00004001, 0x00000000, 0x00100e46, 0x00000000, 0x04000036,
        0x00100072, 0x00000000, 0x00020246, 0x04000036, 0x00100082, 0x00000000, 0x0002200a, 0x090000a8,
        0x0011e0f2, 0x00000000, 0x0010000a, 0x00000001, 0x00004001, 0x00000010, 0x00100e46, 0x00000000,
        0x080000a8, 0x0011e032, 0x00000000, 0x0010000a, 0x00000001, 0x00004001, 0x00000020, 0x00022596,
        0x0100003e,
    };

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 0;
    root_parameters[1].Constants.RegisterSpace = 0;
    root_parameters[1].Constants.Num32BitValues = 4;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    resource = create_default_buffer(device, 10240,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap);
    gpu_descriptor_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap);

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = 256;
    uav_desc.Buffer.StructureByteStride = 40;
    ID3D12Device_CreateUnorderedAccessView(device, resource, NULL, &uav_desc, cpu_descriptor_handle);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    dimensions.x = 2;
    dimensions.y = 3;
    dimensions.z = 1;
    dimensions.w = 0;
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(command_list, 1, 4, &dimensions, 0);
    ID3D12GraphicsCommandList_Dispatch(command_list, dimensions.x, dimensions.y, dimensions.z);

    transition_resource_state(command_list, resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(resource, uav_desc.Format, &rb, queue, command_list);
    i = 0;
    data = rb.data;
    for (y = 0; y < dimensions.y; ++y)
    {
        for (group_y = 0; group_y < 2; ++group_y)
        {
            for (x = 0; x < dimensions.x; ++x)
            {
                for (group_x = 0; group_x < 3; ++group_x)
                {
                    const unsigned int dispatch_id[2] = {x * 3 + group_x, y * 2 + group_y};
                    const unsigned int group_index = group_y * 3 + group_x;
                    const struct data *d = &data[i];

                    ok(d->group_id[0] == x && d->group_id[1] == y && !d->group_id[2],
                            "Got group id (%u, %u, %u), expected (%u, %u, %u) at %u (%u, %u, %u, %u).\n",
                            d->group_id[0], d->group_id[1], d->group_id[2], x, y, 0,
                            i, x, y, group_x, group_y);
                    ok(d->group_index == group_index,
                            "Got group index %u, expected %u at %u (%u, %u, %u, %u).\n",
                            d->group_index, group_index, i, x, y, group_x, group_y);
                    ok(d->dispatch_id[0] == dispatch_id[0] && d->dispatch_id[1] == dispatch_id[1]
                            && !d->dispatch_id[2],
                            "Got dispatch id (%u, %u, %u), expected (%u, %u, %u) "
                            "at %u (%u, %u, %u, %u).\n",
                            d->dispatch_id[0], d->dispatch_id[1], d->dispatch_id[2],
                            dispatch_id[0], dispatch_id[1], 0,
                            i, x, y, group_x, group_y);
                    ok(d->thread_id[0] == group_x && d->thread_id[1] == group_y && !d->thread_id[2],
                            "Got group thread id (%u, %u, %u), expected (%u, %u, %u) "
                            "at %u (%u, %u, %u, %u).\n",
                            d->thread_id[0], d->thread_id[1], d->thread_id[2], group_x, group_y, 0,
                            i, x, y, group_x, group_y);
                    ++i;
                }
            }
        }
    }
    release_resource_readback(&rb);

    ID3D12DescriptorHeap_Release(descriptor_heap);
    ID3D12Resource_Release(resource);
    destroy_test_context(&context);
}

static void test_tgsm(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor_handle;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    ID3D12DescriptorHeap *cpu_descriptor_heap;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[1];
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12Resource *buffer, *buffer2;
    unsigned int descriptor_size;
    unsigned int data, expected;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    float float_data;
    unsigned int i;
    HRESULT hr;

    static const DWORD raw_tgsm_code[] =
    {
#if 0
        RWByteAddressBuffer u;
        groupshared uint m;

        [numthreads(32, 1, 1)]
        void main(uint local_idx : SV_GroupIndex, uint group_id : SV_GroupID)
        {
            if (!local_idx)
                m = group_id.x;
            GroupMemoryBarrierWithGroupSync();
            InterlockedAdd(m, group_id.x);
            GroupMemoryBarrierWithGroupSync();
            if (!local_idx)
                u.Store(4 * group_id.x, m);
        }
#endif
        0x43425844, 0x467df6d9, 0x5f56edda, 0x5c96b787, 0x60c91fb8, 0x00000001, 0x00000148, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000000f4, 0x00050050, 0x0000003d, 0x0100086a,
        0x0300009d, 0x0011e000, 0x00000000, 0x0200005f, 0x00024000, 0x0200005f, 0x00021012, 0x02000068,
        0x00000001, 0x0400009f, 0x0011f000, 0x00000000, 0x00000004, 0x0400009b, 0x00000020, 0x00000001,
        0x00000001, 0x0200001f, 0x0002400a, 0x060000a6, 0x0011f012, 0x00000000, 0x00004001, 0x00000000,
        0x0002100a, 0x01000015, 0x010018be, 0x060000ad, 0x0011f000, 0x00000000, 0x00004001, 0x00000000,
        0x0002100a, 0x010018be, 0x0200001f, 0x0002400a, 0x06000029, 0x00100012, 0x00000000, 0x0002100a,
        0x00004001, 0x00000002, 0x070000a5, 0x00100022, 0x00000000, 0x00004001, 0x00000000, 0x0011f006,
        0x00000000, 0x070000a6, 0x0011e012, 0x00000000, 0x0010000a, 0x00000000, 0x0010001a, 0x00000000,
        0x01000015, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE cs_raw_tgsm = {raw_tgsm_code, sizeof(raw_tgsm_code)};
    static const DWORD structured_tgsm_code[] =
    {
#if 0
        #define GROUP_SIZE 32

        RWByteAddressBuffer u;
        RWByteAddressBuffer u2;
        groupshared uint m[GROUP_SIZE];

        [numthreads(GROUP_SIZE, 1, 1)]
        void main(uint local_idx : SV_GroupIndex, uint group_id : SV_GroupID)
        {
            uint sum, original, i;

            if (!local_idx)
            {
                for (i = 0; i < GROUP_SIZE; ++i)
                    m[i] = 2 * group_id.x;
            }
            GroupMemoryBarrierWithGroupSync();
            InterlockedAdd(m[local_idx], 1);
            GroupMemoryBarrierWithGroupSync();
            for (i = 0, sum = 0; i < GROUP_SIZE; sum += m[i++]);
            u.InterlockedExchange(4 * group_id.x, sum, original);
            u2.Store(4 * group_id.x, original);
        }
#endif
        0x43425844, 0x9d906c94, 0x81f5ad92, 0x11e860b2, 0x3623c824, 0x00000001, 0x000002c0, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x0000026c, 0x00050050, 0x0000009b, 0x0100086a,
        0x0300009d, 0x0011e000, 0x00000000, 0x0300009d, 0x0011e000, 0x00000001, 0x0200005f, 0x00024000,
        0x0200005f, 0x00021012, 0x02000068, 0x00000002, 0x050000a0, 0x0011f000, 0x00000000, 0x00000004,
        0x00000020, 0x0400009b, 0x00000020, 0x00000001, 0x00000001, 0x0200001f, 0x0002400a, 0x06000029,
        0x00100012, 0x00000000, 0x0002100a, 0x00004001, 0x00000001, 0x05000036, 0x00100022, 0x00000000,
        0x00004001, 0x00000000, 0x01000030, 0x07000050, 0x00100042, 0x00000000, 0x0010001a, 0x00000000,
        0x00004001, 0x00000020, 0x03040003, 0x0010002a, 0x00000000, 0x090000a8, 0x0011f012, 0x00000000,
        0x0010001a, 0x00000000, 0x00004001, 0x00000000, 0x0010000a, 0x00000000, 0x0700001e, 0x00100022,
        0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x00000001, 0x01000016, 0x01000015, 0x010018be,
        0x04000036, 0x00100012, 0x00000000, 0x0002400a, 0x05000036, 0x00100022, 0x00000000, 0x00004001,
        0x00000000, 0x070000ad, 0x0011f000, 0x00000000, 0x00100046, 0x00000000, 0x00004001, 0x00000001,
        0x010018be, 0x08000036, 0x00100032, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x01000030, 0x07000050, 0x00100042, 0x00000000, 0x0010001a, 0x00000000, 0x00004001,
        0x00000020, 0x03040003, 0x0010002a, 0x00000000, 0x0700001e, 0x00100022, 0x00000001, 0x0010001a,
        0x00000000, 0x00004001, 0x00000001, 0x090000a7, 0x00100042, 0x00000000, 0x0010001a, 0x00000000,
        0x00004001, 0x00000000, 0x0011f006, 0x00000000, 0x0700001e, 0x00100012, 0x00000001, 0x0010000a,
        0x00000000, 0x0010002a, 0x00000000, 0x05000036, 0x00100032, 0x00000000, 0x00100046, 0x00000001,
        0x01000016, 0x06000029, 0x00100022, 0x00000000, 0x0002100a, 0x00004001, 0x00000002, 0x090000b8,
        0x00100012, 0x00000001, 0x0011e000, 0x00000000, 0x0010001a, 0x00000000, 0x0010000a, 0x00000000,
        0x070000a6, 0x0011e012, 0x00000001, 0x0010001a, 0x00000000, 0x0010000a, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE cs_structured_tgsm = {structured_tgsm_code, sizeof(structured_tgsm_code)};
    static const DWORD structured_tgsm_float_code[] =
    {
#if 0
        #define GROUP_SIZE 32

        struct data
        {
            float f;
            uint u;
        };

        RWBuffer<float> u;
        RWBuffer<uint> u2;
        groupshared data m[GROUP_SIZE];

        [numthreads(GROUP_SIZE, 1, 1)]
        void main(uint local_idx : SV_GroupIndex, uint group_id : SV_GroupID,
                uint thread_id : SV_DispatchThreadID)
        {
            uint i;
            if (!local_idx)
            {
                for (i = 0; i < GROUP_SIZE; ++i)
                {
                    m[i].f = group_id.x;
                    m[i].u = group_id.x;
                }
            }
            GroupMemoryBarrierWithGroupSync();
            for (i = 0; i < local_idx; ++i)
            {
                m[local_idx].f += group_id.x;
                m[local_idx].u += group_id.x;
            }
            u[thread_id.x] = m[local_idx].f;
            u2[thread_id.x] = m[local_idx].u;
        }
#endif
        0x43425844, 0xaadf1a71, 0x16f60224, 0x89b6ce76, 0xb66fb96f, 0x00000001, 0x000002ac, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000258, 0x00050050, 0x00000096, 0x0100086a,
        0x0400089c, 0x0011e000, 0x00000000, 0x00005555, 0x0400089c, 0x0011e000, 0x00000001, 0x00004444,
        0x0200005f, 0x00024000, 0x0200005f, 0x00021012, 0x0200005f, 0x00020012, 0x02000068, 0x00000002,
        0x050000a0, 0x0011f000, 0x00000000, 0x00000008, 0x00000020, 0x0400009b, 0x00000020, 0x00000001,
        0x00000001, 0x0200001f, 0x0002400a, 0x04000056, 0x00100012, 0x00000000, 0x0002100a, 0x04000036,
        0x00100022, 0x00000000, 0x0002100a, 0x05000036, 0x00100042, 0x00000000, 0x00004001, 0x00000000,
        0x01000030, 0x07000050, 0x00100082, 0x00000000, 0x0010002a, 0x00000000, 0x00004001, 0x00000020,
        0x03040003, 0x0010003a, 0x00000000, 0x090000a8, 0x0011f032, 0x00000000, 0x0010002a, 0x00000000,
        0x00004001, 0x00000000, 0x00100046, 0x00000000, 0x0700001e, 0x00100042, 0x00000000, 0x0010002a,
        0x00000000, 0x00004001, 0x00000001, 0x01000016, 0x01000015, 0x010018be, 0x04000056, 0x00100012,
        0x00000000, 0x0002100a, 0x05000036, 0x00100022, 0x00000000, 0x00004001, 0x00000000, 0x01000030,
        0x06000050, 0x00100042, 0x00000000, 0x0010001a, 0x00000000, 0x0002400a, 0x03040003, 0x0010002a,
        0x00000000, 0x080000a7, 0x001000c2, 0x00000000, 0x0002400a, 0x00004001, 0x00000000, 0x0011f406,
        0x00000000, 0x07000000, 0x00100012, 0x00000001, 0x0010000a, 0x00000000, 0x0010002a, 0x00000000,
        0x0600001e, 0x00100022, 0x00000001, 0x0010003a, 0x00000000, 0x0002100a, 0x080000a8, 0x0011f032,
        0x00000000, 0x0002400a, 0x00004001, 0x00000000, 0x00100046, 0x00000001, 0x0700001e, 0x00100022,
        0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x00000001, 0x01000016, 0x080000a7, 0x00100032,
        0x00000000, 0x0002400a, 0x00004001, 0x00000000, 0x0011f046, 0x00000000, 0x060000a4, 0x0011e0f2,
        0x00000000, 0x00020006, 0x00100006, 0x00000000, 0x060000a4, 0x0011e0f2, 0x00000001, 0x00020006,
        0x00100556, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE cs_structured_tgsm_float
            = {structured_tgsm_float_code, sizeof(structured_tgsm_float_code)};
    static const unsigned int zero[4] = {0};

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 2;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    buffer = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    heap_desc.Flags = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cpu_descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap);
    gpu_descriptor_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap);

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = 256;
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(device, buffer, NULL, &uav_desc, cpu_descriptor_handle);
    cpu_descriptor_handle.ptr += descriptor_size;
    ID3D12Device_CreateUnorderedAccessView(device, NULL, NULL, &uav_desc, cpu_descriptor_handle);

    /* cs_raw_tgsm */
    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature, cs_raw_tgsm);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_Dispatch(command_list, 64, 1, 1);

    transition_resource_state(command_list, buffer,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < 64; ++i)
    {
        data = get_readback_uint(&rb, i, 0);
        expected = 33 * i;
        ok(data == expected, "Got %u, expected %u (index %u).\n", data, expected, i);
    }
    release_resource_readback(&rb);

    ID3D12PipelineState_Release(context.pipeline_state);

    /* cs_structured_tgsm */
    reset_command_list(command_list, context.allocator);

    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature, cs_structured_tgsm);

    /* FIXME: Re-creating the buffer should not be needed when
     * ClearUnorderedAccessViewUint() is implemented.
     */
    ID3D12Resource_Release(buffer);
    buffer = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    buffer2 = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap);
    ID3D12Device_CreateUnorderedAccessView(device, buffer, NULL, &uav_desc, cpu_descriptor_handle);
    cpu_descriptor_handle.ptr += descriptor_size;
    ID3D12Device_CreateUnorderedAccessView(device, buffer2, NULL, &uav_desc, cpu_descriptor_handle);

    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_descriptor_heap);
    ID3D12Device_CreateUnorderedAccessView(device, buffer, NULL, &uav_desc, cpu_descriptor_handle);
    cpu_descriptor_handle.ptr += descriptor_size;
    ID3D12Device_CreateUnorderedAccessView(device, buffer2, NULL, &uav_desc, cpu_descriptor_handle);

    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_descriptor_heap);
    gpu_descriptor_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap);
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(command_list,
            gpu_descriptor_handle, cpu_descriptor_handle, buffer, zero, 0, NULL);
    gpu_descriptor_handle.ptr += descriptor_size;
    cpu_descriptor_handle.ptr += descriptor_size;
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(command_list,
            gpu_descriptor_handle, cpu_descriptor_handle, buffer2, zero, 0, NULL);

    gpu_descriptor_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_Dispatch(command_list, 32, 1, 1);

    transition_resource_state(command_list, buffer,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition_resource_state(command_list, buffer2,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < 32; ++i)
    {
        expected = 64 * i + 32;
        data = get_readback_uint(&rb, i, 0);
        ok(data == expected, "Got %u, expected %u (index %u).\n", data, expected, i);
    }
    release_resource_readback(&rb);
    reset_command_list(command_list, context.allocator);
    get_buffer_readback_with_command_list(buffer2, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < 32; ++i)
    {
        expected = 64 * i + 32;
        data = get_readback_uint(&rb, i, 0);
        ok(data == expected || !data, "Got %u, expected %u (index %u).\n", data, expected, i);
    }
    release_resource_readback(&rb);

    ID3D12PipelineState_Release(context.pipeline_state);

    /* cs_structured_tgsm_float */
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, buffer,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition_resource_state(command_list, buffer2,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    context.pipeline_state = create_compute_pipeline_state(device,
            context.root_signature, cs_structured_tgsm_float);

    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap);
    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    uav_desc.Buffer.Flags = 0;
    ID3D12Device_CreateUnorderedAccessView(device, buffer, NULL, &uav_desc, cpu_descriptor_handle);
    cpu_descriptor_handle.ptr += descriptor_size;
    uav_desc.Format = DXGI_FORMAT_R32_UINT;
    ID3D12Device_CreateUnorderedAccessView(device, buffer2, NULL, &uav_desc, cpu_descriptor_handle);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_Dispatch(command_list, 3, 1, 1);

    transition_resource_state(command_list, buffer,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition_resource_state(command_list, buffer2,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < 96; ++i)
    {
        expected = (i % 32 + 1) * (i / 32);
        float_data = get_readback_float(&rb, i, 0);
        ok(float_data == expected, "Got %.8e, expected %u (index %u).\n", float_data, expected, i);
    }
    release_resource_readback(&rb);
    reset_command_list(command_list, context.allocator);
    get_buffer_readback_with_command_list(buffer2, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < 96; ++i)
    {
        expected = (i % 32 + 1) * (i / 32);
        data = get_readback_uint(&rb, i, 0);
        ok(data == expected, "Got %u, expected %u (index %u).\n", data, expected, i);
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(buffer);
    ID3D12Resource_Release(buffer2);
    ID3D12DescriptorHeap_Release(cpu_descriptor_heap);
    ID3D12DescriptorHeap_Release(descriptor_heap);
    destroy_test_context(&context);
}

static void test_uav_load(void)
{
    struct texture
    {
        unsigned int width;
        unsigned int height;
        unsigned int miplevel_count;
        unsigned int array_size;
        DXGI_FORMAT format;
        D3D12_SUBRESOURCE_DATA data[3];
    };

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, rtv_float, rtv_uint, rtv_sint;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    ID3D12DescriptorHeap *rtv_heap, *uav_heap;
    D3D12_ROOT_PARAMETER root_parameters[2];
    const D3D12_SHADER_BYTECODE *current_ps;
    ID3D12GraphicsCommandList *command_list;
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
    const struct texture *current_texture;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12Resource *texture, *rt_texture;
    D3D12_RESOURCE_DESC resource_desc;
    D3D12_CLEAR_VALUE clear_value;
    struct test_context_desc desc;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    unsigned int rtv_size;
    ID3D12Device *device;
    unsigned int i, x, y;
    HRESULT hr;

    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const DWORD ps_ld_2d_float_code[] =
    {
#if 0
        RWTexture2D<float> u;

        float main(float4 position : SV_Position) : SV_Target
        {
            float2 s;
            u.GetDimensions(s.x, s.y);
            return u[s * float2(position.x / 640.0f, position.y / 480.0f)];
        }
#endif
        0x43425844, 0xd5996e04, 0x6bede909, 0x0a7ad18e, 0x5eb277fb, 0x00000001, 0x00000194, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x00000e01, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000f8, 0x00000050,
        0x0000003e, 0x0100086a, 0x0400189c, 0x0011e000, 0x00000001, 0x00005555, 0x04002064, 0x00101032,
        0x00000000, 0x00000001, 0x03000065, 0x00102012, 0x00000000, 0x02000068, 0x00000001, 0x8900003d,
        0x800000c2, 0x00155543, 0x00100032, 0x00000000, 0x00004001, 0x00000000, 0x0011ee46, 0x00000001,
        0x07000038, 0x001000f2, 0x00000000, 0x00100546, 0x00000000, 0x00101546, 0x00000000, 0x0a000038,
        0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x00004002, 0x3acccccd, 0x3b088889, 0x3b088889,
        0x3b088889, 0x0500001c, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x890000a3, 0x800000c2,
        0x00155543, 0x00100012, 0x00000000, 0x00100e46, 0x00000000, 0x0011ee46, 0x00000001, 0x05000036,
        0x00102012, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ld_2d_float = {ps_ld_2d_float_code, sizeof(ps_ld_2d_float_code)};
    static const DWORD ps_ld_2d_uint_code[] =
    {
#if 0
        RWTexture2D<uint> u;

        uint main(float4 position : SV_Position) : SV_Target
        {
            float2 s;
            u.GetDimensions(s.x, s.y);
            return u[s * float2(position.x / 640.0f, position.y / 480.0f)];
        }
#endif
        0x43425844, 0x2cc0af18, 0xb28eca73, 0x9651215b, 0xebe3f361, 0x00000001, 0x00000194, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001,
        0x00000000, 0x00000e01, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000f8, 0x00000050,
        0x0000003e, 0x0100086a, 0x0400189c, 0x0011e000, 0x00000001, 0x00004444, 0x04002064, 0x00101032,
        0x00000000, 0x00000001, 0x03000065, 0x00102012, 0x00000000, 0x02000068, 0x00000001, 0x8900003d,
        0x800000c2, 0x00111103, 0x00100032, 0x00000000, 0x00004001, 0x00000000, 0x0011ee46, 0x00000001,
        0x07000038, 0x001000f2, 0x00000000, 0x00100546, 0x00000000, 0x00101546, 0x00000000, 0x0a000038,
        0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x00004002, 0x3acccccd, 0x3b088889, 0x3b088889,
        0x3b088889, 0x0500001c, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x890000a3, 0x800000c2,
        0x00111103, 0x00100012, 0x00000000, 0x00100e46, 0x00000000, 0x0011ee46, 0x00000001, 0x05000036,
        0x00102012, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ld_2d_uint = {ps_ld_2d_uint_code, sizeof(ps_ld_2d_uint_code)};
    static const DWORD ps_ld_2d_int_code[] =
    {
#if 0
        RWTexture2D<int> u;

        int main(float4 position : SV_Position) : SV_Target
        {
            float2 s;
            u.GetDimensions(s.x, s.y);
            return u[s * float2(position.x / 640.0f, position.y / 480.0f)];
        }
#endif
        0x43425844, 0x7deee248, 0xe7c48698, 0x9454db00, 0x921810e7, 0x00000001, 0x00000194, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000002,
        0x00000000, 0x00000e01, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000f8, 0x00000050,
        0x0000003e, 0x0100086a, 0x0400189c, 0x0011e000, 0x00000001, 0x00003333, 0x04002064, 0x00101032,
        0x00000000, 0x00000001, 0x03000065, 0x00102012, 0x00000000, 0x02000068, 0x00000001, 0x8900003d,
        0x800000c2, 0x000cccc3, 0x00100032, 0x00000000, 0x00004001, 0x00000000, 0x0011ee46, 0x00000001,
        0x07000038, 0x001000f2, 0x00000000, 0x00100546, 0x00000000, 0x00101546, 0x00000000, 0x0a000038,
        0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x00004002, 0x3acccccd, 0x3b088889, 0x3b088889,
        0x3b088889, 0x0500001c, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x890000a3, 0x800000c2,
        0x000cccc3, 0x00100012, 0x00000000, 0x00100e46, 0x00000000, 0x0011ee46, 0x00000001, 0x05000036,
        0x00102012, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ld_2d_int = {ps_ld_2d_int_code, sizeof(ps_ld_2d_int_code)};
    static const DWORD ps_ld_2d_uint_arr_code[] =
    {
#if 0
        RWTexture2DArray<uint> u;

        uint layer;

        uint main(float4 position : SV_Position) : SV_Target
        {
            float3 s;
            u.GetDimensions(s.x, s.y, s.z);
            s.z = layer;
            return u[s * float3(position.x / 640.0f, position.y / 480.0f, 1.0f)];
        }
#endif
        0x43425844, 0xa7630358, 0xd7e7228f, 0xa9f1be03, 0x838554f1, 0x00000001, 0x000001bc, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001,
        0x00000000, 0x00000e01, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000120, 0x00000050,
        0x00000048, 0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0400409c, 0x0011e000,
        0x00000001, 0x00004444, 0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03000065, 0x00102012,
        0x00000000, 0x02000068, 0x00000001, 0x8900003d, 0x80000202, 0x00111103, 0x00100032, 0x00000000,
        0x00004001, 0x00000000, 0x0011ee46, 0x00000001, 0x07000038, 0x00100032, 0x00000000, 0x00100046,
        0x00000000, 0x00101046, 0x00000000, 0x06000056, 0x001000c2, 0x00000000, 0x00208006, 0x00000000,
        0x00000000, 0x0a000038, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x00004002, 0x3acccccd,
        0x3b088889, 0x3f800000, 0x3f800000, 0x0500001c, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000,
        0x890000a3, 0x80000202, 0x00111103, 0x00100012, 0x00000000, 0x00100e46, 0x00000000, 0x0011ee46,
        0x00000001, 0x05000036, 0x00102012, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ld_2d_uint_arr = {ps_ld_2d_uint_arr_code, sizeof(ps_ld_2d_uint_arr_code)};
    static const float float_data[] =
    {
         0.50f,  0.25f,  1.00f,  0.00f,
        -1.00f, -2.00f, -3.00f, -4.00f,
        -0.50f, -0.25f, -1.00f, -0.00f,
         1.00f,  2.00f,  3.00f,  4.00f,
    };
    static const unsigned int uint_data[] =
    {
        0x00, 0x10, 0x20, 0x30,
        0x40, 0x50, 0x60, 0x70,
        0x80, 0x90, 0xa0, 0xb0,
        0xc0, 0xd0, 0xe0, 0xf0,
    };
    static const unsigned int uint_data2[] =
    {
        0xffff, 0xffff, 0xffff, 0xffff,
        0xffff, 0xc000, 0xc000, 0xffff,
        0xffff, 0xc000, 0xc000, 0xffff,
        0xffff, 0xffff, 0xffff, 0xffff,
    };
    static const unsigned int uint_data3[] =
    {
        0xaa, 0xaa, 0xcc, 0xcc,
        0xaa, 0xaa, 0xdd, 0xdd,
        0xbb, 0xbb, 0xee, 0xee,
        0xbb, 0xbb, 0xff, 0xff,
    };
    static const int int_data[] =
    {
          -1, 0x10, 0x20, 0x30,
        0x40, 0x50, 0x60, -777,
        -666, 0x90, -555, 0xb0,
        0xc0, 0xd0, 0xe0, -101,
    };
    static const struct texture float_2d = {4, 4, 1, 1, DXGI_FORMAT_R32_FLOAT,
            {{float_data, 4 * sizeof(*float_data), 0}}};
    static const struct texture uint_2d = {4, 4, 1, 1, DXGI_FORMAT_R32_UINT,
            {{uint_data, 4 * sizeof(*uint_data), 0}}};
    static const struct texture uint2d_arr = {4, 4, 1, 3, DXGI_FORMAT_R32_UINT,
            {{uint_data, 4 * sizeof(*uint_data), 0},
            {uint_data2, 4 * sizeof(*uint_data2), 0},
            {uint_data3, 4 * sizeof(*uint_data3), 0}}};
    static const struct texture int_2d = {4, 4, 1, 1, DXGI_FORMAT_R32_SINT,
            {{int_data, 4 * sizeof(*int_data), 0}}};

    static const struct test
    {
        const D3D12_SHADER_BYTECODE *ps;
        const struct texture *texture;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
        unsigned int constant;
        const DWORD *expected_colors;
    }
    tests[] =
    {
#define TEX_2D       D3D12_UAV_DIMENSION_TEXTURE2D
#define TEX_2D_ARRAY D3D12_UAV_DIMENSION_TEXTURE2DARRAY
#define R32_FLOAT    DXGI_FORMAT_R32_FLOAT
#define R32_UINT     DXGI_FORMAT_R32_UINT
#define R32_SINT     DXGI_FORMAT_R32_SINT
        {&ps_ld_2d_float,    &float_2d,   {R32_FLOAT, TEX_2D, .Texture2D = {0}}, 0, (const DWORD *)float_data},
        {&ps_ld_2d_uint,     &uint_2d,    {R32_UINT,  TEX_2D, .Texture2D = {0}}, 0, (const DWORD *)uint_data},
        {&ps_ld_2d_int,      &int_2d,     {R32_SINT,  TEX_2D, .Texture2D = {0}}, 0, (const DWORD *)int_data},

        {&ps_ld_2d_uint_arr, &uint2d_arr, {R32_UINT, TEX_2D_ARRAY, .Texture2DArray = {0, 0, ~0u}}, 0,
                (const DWORD *)uint_data},
        {&ps_ld_2d_uint_arr, &uint2d_arr, {R32_UINT, TEX_2D_ARRAY, .Texture2DArray = {0, 0, ~0u}}, 1,
                (const DWORD *)uint_data2},
        {&ps_ld_2d_uint_arr, &uint2d_arr, {R32_UINT, TEX_2D_ARRAY, .Texture2DArray = {0, 0, ~0u}}, 2,
                (const DWORD *)uint_data3},
        {&ps_ld_2d_uint_arr, &uint2d_arr, {R32_UINT, TEX_2D_ARRAY, .Texture2DArray = {0, 1, ~0u}}, 0,
                (const DWORD *)uint_data2},
        {&ps_ld_2d_uint_arr, &uint2d_arr, {R32_UINT, TEX_2D_ARRAY, .Texture2DArray = {0, 1, ~0u}}, 1,
                (const DWORD *)uint_data3},
        {&ps_ld_2d_uint_arr, &uint2d_arr, {R32_UINT, TEX_2D_ARRAY, .Texture2DArray = {0, 2, ~0u}}, 0,
                (const DWORD *)uint_data3},
#undef TEX_2D
#undef TEX_2D_ARRAY
#undef R32_FLOAT
#undef R32_UINT
#undef R32_SINT
    };

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 1;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 0;
    root_parameters[1].Constants.RegisterSpace = 0;
    root_parameters[1].Constants.Num32BitValues = 1;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    set_viewport(&context.viewport, 0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    set_rect(&context.scissor_rect, 0, 0, 640, 480);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 640;
    resource_desc.Height = 480;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear_value.Format = DXGI_FORMAT_R32_FLOAT;
    clear_value.Color[0] = 1.0f;
    clear_value.Color[1] = 1.0f;
    clear_value.Color[2] = 1.0f;
    clear_value.Color[3] = 1.0f;
    hr = ID3D12Device_CreateCommittedResource(device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            &IID_ID3D12Resource, (void **)&rt_texture);
    ok(SUCCEEDED(hr), "Failed to create texture, hr %#x.\n", hr);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 3;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    rtv_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    memset(&rtv_desc, 0, sizeof(rtv_desc));
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Device_CreateRenderTargetView(device, rt_texture, &rtv_desc, cpu_handle);
    rtv_float = cpu_handle;
    cpu_handle.ptr += rtv_size;

    rtv_desc.Format = DXGI_FORMAT_R32_UINT;
    ID3D12Device_CreateRenderTargetView(device, rt_texture, &rtv_desc, cpu_handle);
    rtv_uint = cpu_handle;
    cpu_handle.ptr += rtv_size;

    rtv_desc.Format = DXGI_FORMAT_R32_SINT;
    ID3D12Device_CreateRenderTargetView(device, rt_texture, &rtv_desc, cpu_handle);
    rtv_sint = cpu_handle;

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&uav_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    texture = NULL;
    current_ps = NULL;
    current_texture = NULL;
    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        const struct test *test = &tests[i];

        if (current_ps != test->ps)
        {
            if (context.pipeline_state)
                ID3D12PipelineState_Release(context.pipeline_state);
            current_ps = tests[i].ps;
            context.pipeline_state = create_pipeline_state(context.device,
                    context.root_signature, test->uav_desc.Format, NULL, current_ps, NULL);
        }

        if (current_texture != test->texture)
        {
            if (texture)
                ID3D12Resource_Release(texture);

            current_texture = test->texture;

            resource_desc.Width = current_texture->width;
            resource_desc.Height = current_texture->height;
            resource_desc.MipLevels = current_texture->miplevel_count;
            resource_desc.DepthOrArraySize = current_texture->array_size;
            resource_desc.Format = current_texture->format;
            hr = ID3D12Device_CreateCommittedResource(device,
                    &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void **)&texture);
            ok(SUCCEEDED(hr), "Test %u: Failed to create texture, hr %#x.\n", i, hr);

            upload_texture_data(texture, current_texture->data,
                    resource_desc.MipLevels * resource_desc.DepthOrArraySize, queue, command_list);
            reset_command_list(command_list, context.allocator);

            transition_resource_state(command_list, texture,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(uav_heap);
        ID3D12Device_CreateUnorderedAccessView(device, texture, NULL, &test->uav_desc, cpu_handle);

        switch (test->uav_desc.Format)
        {
            default:
                trace("Unhandled format %#x.\n", test->uav_desc.Format);
            case DXGI_FORMAT_R32_FLOAT:
                cpu_handle = rtv_float;
                break;
            case DXGI_FORMAT_R32_UINT:
                cpu_handle = rtv_uint;
                break;
            case DXGI_FORMAT_R32_SINT:
                cpu_handle = rtv_sint;
                break;
        }

        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &uav_heap);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0,
                ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(uav_heap));
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(command_list, 1, test->constant, 0);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &cpu_handle, FALSE, NULL);
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, cpu_handle, white, 0, NULL);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        transition_sub_resource_state(command_list, rt_texture, 0,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        get_texture_readback_with_command_list(rt_texture, 0, &rb, queue, command_list);
        for (y = 0; y < 4; ++y)
        {
            for (x = 0; x < 4; ++x)
            {
                unsigned int expected = test->expected_colors[y * 4 + x];
                unsigned int color = get_readback_uint(&rb, 80 + x * 160, 60 + y * 120);
                ok(compare_color(color, expected, 0),
                        "Test %u: Got 0x%08x, expected 0x%08x at (%u, %u).\n",
                        i, color, expected, x, y);
            }
        }
        release_resource_readback(&rb);

        reset_command_list(command_list, context.allocator);
        transition_sub_resource_state(command_list, rt_texture, 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    ID3D12Resource_Release(texture);

    ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12DescriptorHeap_Release(uav_heap);
    ID3D12Resource_Release(rt_texture);
    destroy_test_context(&context);
}

static void test_cs_uav_store(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor_handle;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    const D3D12_SHADER_BYTECODE *current_shader;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    ID3D12DescriptorHeap *cpu_descriptor_heap;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[2];
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    ID3D12Device *device;
    ID3D12Resource *cb;
    struct vec4 input;
    unsigned int i;
    HRESULT hr;
    RECT rect;

    static const DWORD cs_1_thread_code[] =
    {
#if 0
        RWTexture2D<float> u;

        float value;

        [numthreads(1, 1, 1)]
        void main()
        {
            uint x, y, width, height;
            u.GetDimensions(width, height);
            for (y = 0; y < height; ++y)
            {
                for (x = 0; x < width; ++x)
                    u[uint2(x, y)] = value;
            }
        }
#endif
        0x43425844, 0x6503503a, 0x4cd524e6, 0x2473915d, 0x93cf1201, 0x00000001, 0x000001c8, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000174, 0x00050050, 0x0000005d, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0400189c, 0x0011e000, 0x00000000, 0x00005555,
        0x02000068, 0x00000003, 0x0400009b, 0x00000001, 0x00000001, 0x00000001, 0x8900103d, 0x800000c2,
        0x00155543, 0x00100032, 0x00000000, 0x00004001, 0x00000000, 0x0011ee46, 0x00000000, 0x05000036,
        0x00100042, 0x00000000, 0x00004001, 0x00000000, 0x01000030, 0x07000050, 0x00100082, 0x00000000,
        0x0010002a, 0x00000000, 0x0010001a, 0x00000000, 0x03040003, 0x0010003a, 0x00000000, 0x05000036,
        0x001000e2, 0x00000001, 0x00100aa6, 0x00000000, 0x05000036, 0x00100082, 0x00000000, 0x00004001,
        0x00000000, 0x01000030, 0x07000050, 0x00100012, 0x00000002, 0x0010003a, 0x00000000, 0x0010000a,
        0x00000000, 0x03040003, 0x0010000a, 0x00000002, 0x05000036, 0x00100012, 0x00000001, 0x0010003a,
        0x00000000, 0x080000a4, 0x0011e0f2, 0x00000000, 0x00100e46, 0x00000001, 0x00208006, 0x00000000,
        0x00000000, 0x0700001e, 0x00100082, 0x00000000, 0x0010003a, 0x00000000, 0x00004001, 0x00000001,
        0x01000016, 0x0700001e, 0x00100042, 0x00000000, 0x0010002a, 0x00000000, 0x00004001, 0x00000001,
        0x01000016, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE cs_1_thread = {cs_1_thread_code, sizeof(cs_1_thread_code)};
    static const DWORD cs_1_group_code[] =
    {
#if 0
        RWTexture2D<float> u;

        float value;

        [numthreads(16, 16, 1)]
        void main(uint3 threadID : SV_GroupThreadID)
        {
            uint2 count, size ;
            u.GetDimensions(size.x, size.y);
            count = size / (uint2)16;
            for (uint y = 0; y < count.y; ++y)
                for (uint x = 0; x < count.x; ++x)
                    u[count * threadID.xy + uint2(x, y)] = value;
        }
#endif
        0x43425844, 0x9fb86044, 0x352c196d, 0x92e14094, 0x46bb95a7, 0x00000001, 0x00000218, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000001c4, 0x00050050, 0x00000071, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0400189c, 0x0011e000, 0x00000000, 0x00005555,
        0x0200005f, 0x00022032, 0x02000068, 0x00000004, 0x0400009b, 0x00000010, 0x00000010, 0x00000001,
        0x8900103d, 0x800000c2, 0x00155543, 0x00100032, 0x00000000, 0x00004001, 0x00000000, 0x0011ee46,
        0x00000000, 0x0a000055, 0x001000f2, 0x00000000, 0x00100546, 0x00000000, 0x00004002, 0x00000004,
        0x00000004, 0x00000004, 0x00000004, 0x05000036, 0x00100012, 0x00000001, 0x00004001, 0x00000000,
        0x01000030, 0x07000050, 0x00100022, 0x00000001, 0x0010000a, 0x00000001, 0x0010003a, 0x00000000,
        0x03040003, 0x0010001a, 0x00000001, 0x05000036, 0x001000e2, 0x00000002, 0x00100006, 0x00000001,
        0x05000036, 0x00100022, 0x00000001, 0x00004001, 0x00000000, 0x01000030, 0x07000050, 0x00100042,
        0x00000001, 0x0010001a, 0x00000001, 0x0010000a, 0x00000000, 0x03040003, 0x0010002a, 0x00000001,
        0x05000036, 0x00100012, 0x00000002, 0x0010001a, 0x00000001, 0x08000023, 0x001000f2, 0x00000003,
        0x00100e46, 0x00000000, 0x00022546, 0x00100e46, 0x00000002, 0x080000a4, 0x0011e0f2, 0x00000000,
        0x00100e46, 0x00000003, 0x00208006, 0x00000000, 0x00000000, 0x0700001e, 0x00100022, 0x00000001,
        0x0010001a, 0x00000001, 0x00004001, 0x00000001, 0x01000016, 0x0700001e, 0x00100012, 0x00000001,
        0x0010000a, 0x00000001, 0x00004001, 0x00000001, 0x01000016, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE cs_1_group = {cs_1_group_code, sizeof(cs_1_group_code)};
    static const DWORD cs_1_store_code[] =
    {
#if 0
        RWTexture2D<float> u;

        float value;

        [numthreads(1, 1, 1)]
        void main(uint3 groupID : SV_GroupID)
        {
            u[groupID.xy] = value;
        }
#endif
        0x43425844, 0xc3add41b, 0x67df51b1, 0x2b887930, 0xcb1ee991, 0x00000001, 0x000000b8, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000064, 0x00050050, 0x00000019, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0400189c, 0x0011e000, 0x00000000, 0x00005555,
        0x0200005f, 0x00021032, 0x0400009b, 0x00000001, 0x00000001, 0x00000001, 0x070000a4, 0x0011e0f2,
        0x00000000, 0x00021546, 0x00208006, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE cs_1_store = {cs_1_store_code, sizeof(cs_1_store_code)};
    static const DWORD cs_dispatch_id_code[] =
    {
#if 0
        RWTexture2D<float> u;

        float value;

        [numthreads(4, 4, 1)]
        void main(uint3 id : SV_DispatchThreadID)
        {
            u[id.xy] = value;
        }
#endif
        0x43425844, 0x60166991, 0x4b595266, 0x7fb67d79, 0x485c4f0d, 0x00000001, 0x000000b8, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000064, 0x00050050, 0x00000019, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0400189c, 0x0011e000, 0x00000000, 0x00005555,
        0x0200005f, 0x00020032, 0x0400009b, 0x00000004, 0x00000004, 0x00000001, 0x070000a4, 0x0011e0f2,
        0x00000000, 0x00020546, 0x00208006, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE cs_dispatch_id = {cs_dispatch_id_code, sizeof(cs_dispatch_id_code)};
    static const DWORD cs_group_index_code[] =
    {
#if 0
        RWTexture2D<float> u;

        float value;

        [numthreads(32, 1, 1)]
        void main(uint index : SV_GroupIndex)
        {
            uint2 size;
            u.GetDimensions(size.x, size.y);
            uint count = size.x * size.y / 32;
            index *= count;
            for (uint i = 0; i < count; ++i, ++index)
                u[uint2(index % size.x, index / size.x)] = value;
        }
#endif
        0x43425844, 0xb685a70f, 0x94c2f263, 0x4f1d8eaa, 0xeab65731, 0x00000001, 0x000001f8, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000001a4, 0x00050050, 0x00000069, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0400189c, 0x0011e000, 0x00000000, 0x00005555,
        0x0200005f, 0x00024000, 0x02000068, 0x00000004, 0x0400009b, 0x00000020, 0x00000001, 0x00000001,
        0x8900103d, 0x800000c2, 0x00155543, 0x00100032, 0x00000000, 0x00004001, 0x00000000, 0x0011ee46,
        0x00000000, 0x08000026, 0x0000d000, 0x00100022, 0x00000000, 0x0010000a, 0x00000000, 0x0010001a,
        0x00000000, 0x07000055, 0x00100022, 0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x00000005,
        0x07000026, 0x0000d000, 0x00100042, 0x00000000, 0x0002400a, 0x0010001a, 0x00000000, 0x05000036,
        0x00100012, 0x00000001, 0x0010002a, 0x00000000, 0x05000036, 0x00100022, 0x00000001, 0x00004001,
        0x00000000, 0x01000030, 0x07000050, 0x00100082, 0x00000000, 0x0010001a, 0x00000001, 0x0010001a,
        0x00000000, 0x03040003, 0x0010003a, 0x00000000, 0x0900004e, 0x00100012, 0x00000002, 0x00100012,
        0x00000003, 0x0010000a, 0x00000001, 0x0010000a, 0x00000000, 0x05000036, 0x001000e2, 0x00000003,
        0x00100006, 0x00000002, 0x080000a4, 0x0011e0f2, 0x00000000, 0x00100e46, 0x00000003, 0x00208006,
        0x00000000, 0x00000000, 0x0a00001e, 0x00100032, 0x00000001, 0x00100046, 0x00000001, 0x00004002,
        0x00000001, 0x00000001, 0x00000000, 0x00000000, 0x01000016, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE cs_group_index = {cs_group_index_code, sizeof(cs_group_index_code)};
    static const float zero[4] = {};
    static const struct
    {
        const D3D12_SHADER_BYTECODE *shader;
        float value;
    }
    tests[] =
    {
        {&cs_1_thread,    1.0f},
        {&cs_1_thread,    0.5f},
        {&cs_1_group,     2.0f},
        {&cs_1_group,     4.0f},
        {&cs_group_index, 0.3f},
        {&cs_group_index, 0.1f},
    };

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    cb = create_upload_buffer(context.device, sizeof(input), NULL);

    resource = create_default_texture(device, 64, 64, DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    heap_desc.Flags = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cpu_descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap);
    gpu_descriptor_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap);

    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0;
    uav_desc.Texture2D.PlaneSlice = 0;
    ID3D12Device_CreateUnorderedAccessView(device, resource, NULL, &uav_desc, cpu_descriptor_handle);
    cpu_descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_descriptor_heap);
    ID3D12Device_CreateUnorderedAccessView(device, resource, NULL, &uav_desc, cpu_descriptor_handle);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Failed to close command list, hr %#x.\n", hr);

    current_shader = NULL;
    pipeline_state = NULL;
    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        if (current_shader != tests[i].shader)
        {
            if (pipeline_state)
                ID3D12PipelineState_Release(pipeline_state);

            current_shader = tests[i].shader;
            pipeline_state = create_compute_pipeline_state(device, root_signature, *current_shader);
        }

        memset(&input, 0, sizeof(input));
        input.x = tests[i].value;
        update_buffer_data(cb, 0, sizeof(input), &input.x);

        reset_command_list(command_list, context.allocator);
        transition_sub_resource_state(command_list, resource, 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(command_list,
                gpu_descriptor_handle, cpu_descriptor_handle, resource, zero, 0, NULL);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
        ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
        ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list, 1,
                ID3D12Resource_GetGPUVirtualAddress(cb));
        ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
        ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

        transition_sub_resource_state(command_list, resource, 0,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check_sub_resource_float(resource, 0, queue, command_list, tests[i].value, 2);
    }

    ID3D12PipelineState_Release(pipeline_state);
    pipeline_state = create_compute_pipeline_state(device, root_signature, cs_1_store);

    memset(&input, 0, sizeof(input));
    input.x = 1.0f;
    update_buffer_data(cb, 0, sizeof(input), &input.x);

    reset_command_list(command_list, context.allocator);
    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(command_list,
            gpu_descriptor_handle, cpu_descriptor_handle, resource, zero, 0, NULL);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_Dispatch(command_list, 64, 64, 1);

    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_float(resource, 0, queue, command_list, 1.0f, 2);

    memset(&input, 0, sizeof(input));
    input.x = 0.5f;
    update_buffer_data(cb, 0, sizeof(input), &input.x);

    reset_command_list(command_list, context.allocator);
    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_Dispatch(command_list, 16, 32, 1);

    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(resource, 0, &rb, queue, command_list);
    set_rect(&rect, 0, 0, 16, 32);
    check_readback_data_float(&rb, &rect, 0.5f, 2);
    set_rect(&rect, 0, 32, rb.width, rb.height);
    check_readback_data_float(&rb, &rect, 1.0f, 2);
    set_rect(&rect, 16, 0, rb.width, rb.height);
    check_readback_data_float(&rb, &rect, 1.0f, 2);
    release_resource_readback(&rb);

    ID3D12PipelineState_Release(pipeline_state);
    pipeline_state = create_compute_pipeline_state(device, root_signature, cs_dispatch_id);

    memset(&input, 0, sizeof(input));
    input.x = 0.6f;
    update_buffer_data(cb, 0, sizeof(input), &input.x);

    reset_command_list(command_list, context.allocator);
    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_Dispatch(command_list, 15, 15, 1);

    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(resource, 0, &rb, queue, command_list);
    set_rect(&rect, 0, 0, 60, 60);
    check_readback_data_float(&rb, &rect, 0.6f, 2);
    set_rect(&rect, 0, 60, rb.width, rb.height);
    check_readback_data_float(&rb, &rect, 1.0f, 2);
    set_rect(&rect, 60, 0, rb.width, rb.height);
    check_readback_data_float(&rb, &rect, 1.0f, 2);
    release_resource_readback(&rb);

    memset(&input, 0, sizeof(input));
    input.x = 0.7f;
    update_buffer_data(cb, 0, sizeof(input), &input.x);

    reset_command_list(command_list, context.allocator);
    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0, gpu_descriptor_handle);
    ID3D12GraphicsCommandList_Dispatch(command_list, 16, 16, 1);

    transition_sub_resource_state(command_list, resource, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_float(resource, 0, queue, command_list, 0.7f, 2);

    ID3D12Resource_Release(cb);
    ID3D12Resource_Release(resource);
    ID3D12PipelineState_Release(pipeline_state);
    ID3D12RootSignature_Release(root_signature);
    ID3D12DescriptorHeap_Release(cpu_descriptor_heap);
    ID3D12DescriptorHeap_Release(descriptor_heap);
    destroy_test_context(&context);
}

static unsigned int read_uav_counter(const struct test_context *context,
        ID3D12Resource *counter_buffer, size_t offset)
{
    struct resource_readback rb;
    uint32_t counter;

    transition_sub_resource_state(context->list, counter_buffer, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(counter_buffer, DXGI_FORMAT_R32_UINT, &rb,
            context->queue, context->list);
    counter = get_readback_uint(&rb, offset / sizeof(counter), 0);
    release_resource_readback(&rb);
    reset_command_list(context->list, context->allocator);
    transition_sub_resource_state(context->list, counter_buffer, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return counter;
}

static int compare_id(const void *a, const void *b)
{
    return *(int *)a - *(int *)b;
}

static void test_uav_counters(void)
{
    ID3D12Resource *buffer, *buffer2, *counter_buffer;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[1];
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    uint32_t data, id[128];
    ID3D12Device *device;
    uint32_t counter;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_producer_code[] =
    {
#if 0
        RWStructuredBuffer<uint> u;

        [numthreads(4, 1, 1)]
        void main(uint3 dispatch_id : SV_DispatchThreadID)
        {
            uint counter = u.IncrementCounter();
            u[counter] = dispatch_id.x;
        }
#endif
        0x43425844, 0x013163a8, 0xe7d371b8, 0x4f71e39a, 0xd479e584, 0x00000001, 0x000000c8, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000074, 0x00050050, 0x0000001d, 0x0100086a,
        0x0480009e, 0x0011e000, 0x00000000, 0x00000004, 0x0200005f, 0x00020012, 0x02000068, 0x00000001,
        0x0400009b, 0x00000004, 0x00000001, 0x00000001, 0x050000b2, 0x00100012, 0x00000000, 0x0011e000,
        0x00000000, 0x080000a8, 0x0011e012, 0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x00000000,
        0x0002000a, 0x0100003e,
    };
    static const DWORD cs_consumer_code[] =
    {
#if 0
        RWStructuredBuffer<uint> u;
        RWStructuredBuffer<uint> u2;

        [numthreads(4, 1, 1)]
        void main()
        {
            uint counter = u.DecrementCounter();
            u2[counter] = u[counter];
        }
#endif
        0x43425844, 0x957ef3dd, 0x9f317559, 0x09c8f12d, 0xdbfd98c8, 0x00000001, 0x00000100, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000000ac, 0x00050050, 0x0000002b, 0x0100086a,
        0x0480009e, 0x0011e000, 0x00000000, 0x00000004, 0x0400009e, 0x0011e000, 0x00000001, 0x00000004,
        0x02000068, 0x00000001, 0x0400009b, 0x00000004, 0x00000001, 0x00000001, 0x050000b3, 0x00100012,
        0x00000000, 0x0011e000, 0x00000000, 0x8b0000a7, 0x80002302, 0x00199983, 0x00100022, 0x00000000,
        0x0010000a, 0x00000000, 0x00004001, 0x00000000, 0x0011e006, 0x00000000, 0x090000a8, 0x0011e012,
        0x00000001, 0x0010000a, 0x00000000, 0x00004001, 0x00000000, 0x0010001a, 0x00000000, 0x0100003e,
    };

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 2;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_producer_code, sizeof(cs_producer_code)));

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 3;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    buffer = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    buffer2 = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    counter_buffer = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.NumElements = 256;
    uav_desc.Buffer.StructureByteStride = sizeof(uint32_t);
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    ID3D12Device_CreateUnorderedAccessView(device, buffer, counter_buffer, &uav_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 0));
    ID3D12Device_CreateUnorderedAccessView(device, buffer2, NULL, &uav_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 1));

    counter = 0;
    upload_buffer_data(counter_buffer, 0, sizeof(counter), &counter, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_sub_resource_state(command_list, counter_buffer, 0,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    /* produce */
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0,
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap));
    ID3D12GraphicsCommandList_Dispatch(command_list, 16, 1, 1);

    counter = read_uav_counter(&context, counter_buffer, 0);
    ok(counter == 64, "Got unexpected value %u.\n", counter);
    transition_sub_resource_state(command_list, buffer, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    memcpy(id, rb.data, 64 * sizeof(*id));
    release_resource_readback(&rb);
    qsort(id, 64, sizeof(*id), compare_id);
    for (i = 0; i < 64; ++i)
    {
        if (id[i] != i)
            break;
    }
    ok(i == 64, "Got unexpected id %u at %u.\n", id[i], i);

    reset_command_list(command_list, context.allocator);
    transition_sub_resource_state(command_list, buffer, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12PipelineState_Release(context.pipeline_state);
    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_consumer_code, sizeof(cs_consumer_code)));

    /* consume */
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0,
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap));
    ID3D12GraphicsCommandList_Dispatch(command_list, 16, 1, 1);

    transition_sub_resource_state(command_list, buffer2, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    counter = read_uav_counter(&context, counter_buffer, 0);
    ok(!counter, "Got unexpected value %u.\n", counter);
    get_buffer_readback_with_command_list(buffer2, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    memcpy(id, rb.data, 64 * sizeof(*id));
    release_resource_readback(&rb);
    qsort(id, 64, sizeof(*id), compare_id);
    for (i = 0; i < 64; ++i)
    {
        if (id[i] != i)
            break;
    }
    ok(i == 64, "Got unexpected id %u at %u.\n", id[i], i);

    reset_command_list(command_list, context.allocator);
    transition_sub_resource_state(command_list, counter_buffer, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    transition_sub_resource_state(command_list, buffer, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    transition_sub_resource_state(command_list, buffer2, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    /* produce on CPU */
    counter = 8;
    for (i = 0; i < counter; ++i)
        id[i] = 0xdeadbeef;
    upload_buffer_data(buffer, 0, counter * sizeof(*id), id, queue, command_list);
    reset_command_list(command_list, context.allocator);
    upload_buffer_data(counter_buffer, 0, sizeof(counter), &counter, queue, command_list);
    reset_command_list(command_list, context.allocator);

    transition_sub_resource_state(command_list, counter_buffer, 0,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition_sub_resource_state(command_list, buffer, 0,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    /* consume */
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0,
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap));
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    transition_sub_resource_state(command_list, buffer2, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    counter = read_uav_counter(&context, counter_buffer, 0);
    ok(!counter, "Got unexpected value %u.\n", counter);

    get_buffer_readback_with_command_list(buffer2, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < 8; ++i)
    {
        data = get_readback_uint(&rb, i, 0);
        ok(data == 0xdeadbeef, "Got data %u at %u.\n", data, i);
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(buffer);
    ID3D12Resource_Release(buffer2);
    ID3D12Resource_Release(counter_buffer);
    ID3D12DescriptorHeap_Release(descriptor_heap);
    destroy_test_context(&context);
}

static void test_decrement_uav_counter(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    ID3D12Resource *buffer, *counter_buffer;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[1];
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    uint32_t counter;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        RWStructuredBuffer<uint> u;

        [numthreads(1, 1, 1)]
        void main()
        {
            u[0] = u.DecrementCounter();
        }
#endif
        0x43425844, 0xef99d2a2, 0x3141da00, 0x1db3fb8b, 0xa1fc2ee5, 0x00000001, 0x000000c4, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000070, 0x00050050, 0x0000001c, 0x0100086a,
        0x0480009e, 0x0011e000, 0x00000000, 0x00000004, 0x02000068, 0x00000001, 0x0400009b, 0x00000001,
        0x00000001, 0x00000001, 0x050000b3, 0x00100012, 0x00000000, 0x0011e000, 0x00000000, 0x090000a8,
        0x0011e012, 0x00000000, 0x00004001, 0x00000000, 0x00004001, 0x00000000, 0x0010000a, 0x00000000,
        0x0100003e,
    };
    static const struct
    {
        uint32_t initial_value;
        unsigned int decrement_count;
        uint32_t expected_value;
    }
    tests[] =
    {
        {0x00000000,  1, 0xffffffff},
        {0x00000001,  1, 0x00000000},
        {0xffffffff,  1, 0xfffffffe},
        {0x00000010, 16, 0x00000000},
        {0x00000010, 17, 0xffffffff},
    };

    if (!init_compute_test_context(&context))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    buffer = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    counter_buffer = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.NumElements = 256;
    uav_desc.Buffer.StructureByteStride = sizeof(uint32_t);
    uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    ID3D12Device_CreateUnorderedAccessView(device, buffer, counter_buffer, &uav_desc,
            get_cpu_descriptor_handle(&context, descriptor_heap, 0));

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        transition_sub_resource_state(command_list, counter_buffer, 0,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        counter = tests[i].initial_value;
        upload_buffer_data(counter_buffer, 0, sizeof(counter), &counter, queue, command_list);
        reset_command_list(command_list, context.allocator);
        transition_sub_resource_state(command_list, counter_buffer, 0,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list, 0,
                ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap));
        ID3D12GraphicsCommandList_Dispatch(command_list, tests[i].decrement_count, 1, 1);

        counter = read_uav_counter(&context, counter_buffer, 0);
        ok(counter == tests[i].expected_value, "Got %u, expected %u.\n",
                counter, tests[i].expected_value);
    }

    ID3D12Resource_Release(buffer);
    ID3D12Resource_Release(counter_buffer);
    ID3D12DescriptorHeap_Release(descriptor_heap);
    destroy_test_context(&context);
}

static void test_atomic_instructions(void)
{
    ID3D12Resource *ps_buffer, *cs_buffer, *cs_buffer2;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[3];
    ID3D12PipelineState *pipeline_state;
    struct test_context_desc desc;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    unsigned int i, j;
    HRESULT hr;

    static const DWORD ps_atomics_code[] =
    {
#if 0
        RWByteAddressBuffer u;

        uint4 v;
        int4 i;

        void main()
        {
            u.InterlockedAnd(0 * 4, v.x);
            u.InterlockedCompareStore(1 * 4, v.y, v.x);
            u.InterlockedAdd(2 * 4, v.x);
            u.InterlockedOr(3 * 4, v.x);
            u.InterlockedMax(4 * 4, i.x);
            u.InterlockedMin(5 * 4, i.x);
            u.InterlockedMax(6 * 4, v.x);
            u.InterlockedMin(7 * 4, v.x);
            u.InterlockedXor(8 * 4, v.x);
        }
#endif
        0x43425844, 0x24c6a30c, 0x2ce4437d, 0xdee8a0df, 0xd18cb4bc, 0x00000001, 0x000001ac, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000158, 0x00000050, 0x00000056, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x0300009d, 0x0011e000, 0x00000000, 0x080000a9,
        0x0011e000, 0x00000000, 0x00004001, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0b0000ac,
        0x0011e000, 0x00000000, 0x00004001, 0x00000004, 0x0020801a, 0x00000000, 0x00000000, 0x0020800a,
        0x00000000, 0x00000000, 0x080000ad, 0x0011e000, 0x00000000, 0x00004001, 0x00000008, 0x0020800a,
        0x00000000, 0x00000000, 0x080000aa, 0x0011e000, 0x00000000, 0x00004001, 0x0000000c, 0x0020800a,
        0x00000000, 0x00000000, 0x080000ae, 0x0011e000, 0x00000000, 0x00004001, 0x00000010, 0x0020800a,
        0x00000000, 0x00000001, 0x080000af, 0x0011e000, 0x00000000, 0x00004001, 0x00000014, 0x0020800a,
        0x00000000, 0x00000001, 0x080000b0, 0x0011e000, 0x00000000, 0x00004001, 0x00000018, 0x0020800a,
        0x00000000, 0x00000000, 0x080000b1, 0x0011e000, 0x00000000, 0x00004001, 0x0000001c, 0x0020800a,
        0x00000000, 0x00000000, 0x080000ab, 0x0011e000, 0x00000000, 0x00004001, 0x00000020, 0x0020800a,
        0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_atomics = {ps_atomics_code, sizeof(ps_atomics_code)};
    static const DWORD cs_atomics_code[] =
    {
#if 0
        RWByteAddressBuffer u;
        RWByteAddressBuffer u2;

        uint4 v;
        int4 i;

        [numthreads(1, 1, 1)]
        void main()
        {
            uint r;
            u.InterlockedAnd(0 * 4, v.x, r);
            u2.Store(0 * 4, r);
            u.InterlockedCompareExchange(1 * 4, v.y, v.x, r);
            u2.Store(1 * 4, r);
            u.InterlockedAdd(2 * 4, v.x, r);
            u2.Store(2 * 4, r);
            u.InterlockedOr(3 * 4, v.x, r);
            u2.Store(3 * 4, r);
            u.InterlockedMax(4 * 4, i.x, r);
            u2.Store(4 * 4, r);
            u.InterlockedMin(5 * 4, i.x, r);
            u2.Store(5 * 4, r);
            u.InterlockedMax(6 * 4, v.x, r);
            u2.Store(6 * 4, r);
            u.InterlockedMin(7 * 4, v.x, r);
            u2.Store(7 * 4, r);
            u.InterlockedXor(8 * 4, v.x, r);
            u2.Store(8 * 4, r);
        }
#endif
        0x43425844, 0x859a96e3, 0x1a35e463, 0x1e89ce58, 0x5cfe430a, 0x00000001, 0x0000026c, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000218, 0x00050050, 0x00000086, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x0300009d, 0x0011e000, 0x00000000, 0x0300009d,
        0x0011e000, 0x00000001, 0x02000068, 0x00000001, 0x0400009b, 0x00000001, 0x00000001, 0x00000001,
        0x0a0000b5, 0x00100012, 0x00000000, 0x0011e000, 0x00000000, 0x00004001, 0x00000000, 0x0020800a,
        0x00000000, 0x00000000, 0x0d0000b9, 0x00100022, 0x00000000, 0x0011e000, 0x00000000, 0x00004001,
        0x00000004, 0x0020801a, 0x00000000, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0a0000b4,
        0x00100042, 0x00000000, 0x0011e000, 0x00000000, 0x00004001, 0x00000008, 0x0020800a, 0x00000000,
        0x00000000, 0x0a0000b6, 0x00100082, 0x00000000, 0x0011e000, 0x00000000, 0x00004001, 0x0000000c,
        0x0020800a, 0x00000000, 0x00000000, 0x070000a6, 0x0011e0f2, 0x00000001, 0x00004001, 0x00000000,
        0x00100e46, 0x00000000, 0x0a0000ba, 0x00100012, 0x00000000, 0x0011e000, 0x00000000, 0x00004001,
        0x00000010, 0x0020800a, 0x00000000, 0x00000001, 0x0a0000bb, 0x00100022, 0x00000000, 0x0011e000,
        0x00000000, 0x00004001, 0x00000014, 0x0020800a, 0x00000000, 0x00000001, 0x0a0000bc, 0x00100042,
        0x00000000, 0x0011e000, 0x00000000, 0x00004001, 0x00000018, 0x0020800a, 0x00000000, 0x00000000,
        0x0a0000bd, 0x00100082, 0x00000000, 0x0011e000, 0x00000000, 0x00004001, 0x0000001c, 0x0020800a,
        0x00000000, 0x00000000, 0x070000a6, 0x0011e0f2, 0x00000001, 0x00004001, 0x00000010, 0x00100e46,
        0x00000000, 0x0a0000b7, 0x00100012, 0x00000000, 0x0011e000, 0x00000000, 0x00004001, 0x00000020,
        0x0020800a, 0x00000000, 0x00000000, 0x070000a6, 0x0011e012, 0x00000001, 0x00004001, 0x00000020,
        0x0010000a, 0x00000000, 0x0100003e,
    };
    static D3D12_SHADER_BYTECODE cs_atomics = {cs_atomics_code, sizeof(cs_atomics_code)};
    static const char * const instructions[] =
    {
        "atomic_and", "atomic_cmp_store", "atomic_iadd", "atomic_or",
        "atomic_imax", "atomic_imin", "atomic_umax", "atomic_umin", "atomic_xor",
    };
    static const char * const imm_instructions[] =
    {
        "imm_atomic_and", "imm_atomic_cmp_exch", "imm_atomic_iadd", "imm_atomic_or",
        "imm_atomic_imax", "imm_atomic_imin", "imm_atomic_umax", "imm_atomic_umin", "imm_atomic_xor",
    };
    static const struct test
    {
        struct uvec4 v;
        struct ivec4 i;
        unsigned int input[ARRAY_SIZE(instructions)];
        unsigned int expected_result[ARRAY_SIZE(instructions)];
    }
    tests[] =
    {
        {{ 1,   0 }, {-1}, {0xffff,   0, 1, 0, 0, 0, 0, 0, 0xff }, {     1,   1, 2,   1, 0, ~0u, 1,  0, 0xfe}},
        {{~0u, ~0u}, { 0}, {0xffff, 0xf, 1, 0, 0, 0, 0, 9,   ~0u}, {0xffff, 0xf, 0, ~0u, 0,  0, ~0u, 9,    0}},
    };

    memset(&desc, 0, sizeof(desc));
    desc.rt_width = 1;
    desc.rt_height = 1;
    desc.rt_format = DXGI_FORMAT_R32_FLOAT;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[0].Descriptor.RegisterSpace = 0;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[1].Descriptor.ShaderRegister = 1;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[2].Constants.ShaderRegister = 0;
    root_parameters[2].Constants.RegisterSpace = 0;
    root_parameters[2].Constants.Num32BitValues = 8;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 3;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    ps_buffer = create_default_buffer(device, sizeof(tests->input),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cs_buffer = create_default_buffer(device, sizeof(tests->input),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cs_buffer2 = create_default_buffer(device, sizeof(tests->input),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);

    init_pipeline_state_desc(&pso_desc, context.root_signature, 0, NULL, &ps_atomics, NULL);
    pso_desc.NumRenderTargets = 0;
    hr = ID3D12Device_CreateGraphicsPipelineState(context.device, &pso_desc,
            &IID_ID3D12PipelineState, (void **)&context.pipeline_state);
    ok(SUCCEEDED(hr), "Failed to create graphics pipeline state, hr %#x.\n", hr);

    pipeline_state = create_compute_pipeline_state(device, context.root_signature, cs_atomics);

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        const struct test *test = &tests[i];

        upload_buffer_data(ps_buffer, 0, sizeof(test->input), test->input, queue, command_list);
        reset_command_list(command_list, context.allocator);

        upload_buffer_data(cs_buffer, 0, sizeof(test->input), test->input, queue, command_list);
        reset_command_list(command_list, context.allocator);

        transition_sub_resource_state(command_list, ps_buffer, 0,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition_sub_resource_state(command_list, cs_buffer, 0,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition_sub_resource_state(command_list, cs_buffer2, 0,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(command_list,
                0, ID3D12Resource_GetGPUVirtualAddress(ps_buffer));
        ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(command_list,
                1, ID3D12Resource_GetGPUVirtualAddress(cs_buffer));
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 2, 4, &test->v, 0);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 2, 4, &test->i, 4);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(command_list,
                0, ID3D12Resource_GetGPUVirtualAddress(cs_buffer));
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(command_list,
                1, ID3D12Resource_GetGPUVirtualAddress(cs_buffer2));
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(command_list, 2, 4, &test->v, 0);
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(command_list, 2, 4, &test->i, 4);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
        ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

        transition_sub_resource_state(command_list, ps_buffer, 0,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        get_buffer_readback_with_command_list(ps_buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
        for (j = 0; j < ARRAY_SIZE(instructions); ++j)
        {
            unsigned int value = get_readback_uint(&rb, j, 0);
            unsigned int expected = test->expected_result[j];

            if (test->i.x < 0
                    && (!strcmp(instructions[j], "atomic_imax") || !strcmp(instructions[j], "atomic_imin")))
                todo(value == expected, "Test %u: Got %#x (%d), expected %#x (%d) for '%s' "
                        "with inputs (%u, %u), (%d), %#x (%d).\n",
                        i, value, value, expected, expected, instructions[j],
                        test->v.x, test->v.y, test->i.x, test->input[j], test->input[j]);
            else
                ok(value == expected, "Test %u: Got %#x (%d), expected %#x (%d) for '%s' "
                        "with inputs (%u, %u), (%d), %#x (%d).\n",
                        i, value, value, expected, expected, instructions[j],
                        test->v.x, test->v.y, test->i.x, test->input[j], test->input[j]);
        }
        release_resource_readback(&rb);
        reset_command_list(command_list, context.allocator);

        transition_sub_resource_state(command_list, cs_buffer, 0,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        get_buffer_readback_with_command_list(cs_buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
        for (j = 0; j < ARRAY_SIZE(instructions); ++j)
        {
            BOOL todo_instruction = !strcmp(imm_instructions[j], "imm_atomic_imax")
                    || !strcmp(imm_instructions[j], "imm_atomic_imin");
            unsigned int value = get_readback_uint(&rb, j, 0);
            unsigned int expected = test->expected_result[j];

            if (test->i.x < 0 && todo_instruction)
                todo(value == expected, "Test %u: Got %#x (%d), expected %#x (%d) for '%s' "
                        "with inputs (%u, %u), (%d), %#x (%d).\n",
                        i, value, value, expected, expected, imm_instructions[j],
                        test->v.x, test->v.y, test->i.x, test->input[j], test->input[j]);
            else
                ok(value == expected, "Test %u: Got %#x (%d), expected %#x (%d) for '%s' "
                        "with inputs (%u, %u), (%d), %#x (%d).\n",
                        i, value, value, expected, expected, imm_instructions[j],
                        test->v.x, test->v.y, test->i.x, test->input[j], test->input[j]);
        }
        release_resource_readback(&rb);
        reset_command_list(command_list, context.allocator);

        transition_sub_resource_state(command_list, cs_buffer2, 0,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        get_buffer_readback_with_command_list(cs_buffer2, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
        for (j = 0; j < ARRAY_SIZE(instructions); ++j)
        {
            unsigned int out_value = get_readback_uint(&rb, j, 0);
            ok(out_value == test->input[j], "Got original value %u, expected %u for '%s'.\n",
                    out_value, test->input[j], imm_instructions[j]);
        }
        release_resource_readback(&rb);
        reset_command_list(command_list, context.allocator);

        transition_sub_resource_state(command_list, ps_buffer, 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition_sub_resource_state(command_list, cs_buffer, 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition_sub_resource_state(command_list, cs_buffer2, 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }

    ID3D12Resource_Release(ps_buffer);
    ID3D12Resource_Release(cs_buffer);
    ID3D12Resource_Release(cs_buffer2);
    ID3D12PipelineState_Release(pipeline_state);
    destroy_test_context(&context);
}

static void test_buffer_srv(void)
{
    struct buffer
    {
        unsigned int byte_count;
        unsigned int data_offset;
        const void *data;
        unsigned int structure_byte_stride;
    };

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    const D3D12_SHADER_BYTECODE *current_shader;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[2];
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle;
    ID3D12DescriptorHeap *descriptor_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    const struct buffer *current_buffer;
    unsigned int color, expected_color;
    struct test_context_desc desc;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    ID3D12Resource *buffer;
    ID3D12Device *device;
    unsigned int i, x, y;
    HRESULT hr;

    static const DWORD ps_float4_code[] =
    {
#if 0
        Buffer<float4> b;

        float2 size;

        float4 main(float4 position : SV_POSITION) : SV_Target
        {
            float2 p;
            int2 coords;
            p.x = position.x / 640.0f;
            p.y = position.y / 480.0f;
            coords = int2(p.x * size.x, p.y * size.y);
            return b.Load(coords.y * size.x + coords.x);
        }
#endif
        0x43425844, 0xf10ea650, 0x311f5c38, 0x3a888b7f, 0x58230334, 0x00000001, 0x000001a0, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x5449534f, 0x004e4f49,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x00000104, 0x00000040,
        0x00000041, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x04000858, 0x00107000, 0x00000000,
        0x00005555, 0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x08000038, 0x00100032, 0x00000000, 0x00101516, 0x00000000, 0x00208516,
        0x00000000, 0x00000000, 0x0a000038, 0x00100032, 0x00000000, 0x00100046, 0x00000000, 0x00004002,
        0x3b088889, 0x3acccccd, 0x00000000, 0x00000000, 0x05000043, 0x00100032, 0x00000000, 0x00100046,
        0x00000000, 0x0a000032, 0x00100012, 0x00000000, 0x0010000a, 0x00000000, 0x0020800a, 0x00000000,
        0x00000000, 0x0010001a, 0x00000000, 0x0500001b, 0x00100012, 0x00000000, 0x0010000a, 0x00000000,
        0x0700002d, 0x001020f2, 0x00000000, 0x00100006, 0x00000000, 0x00107e46, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_float4 = {ps_float4_code, sizeof(ps_float4_code)};
    static const DWORD ps_structured_code[] =
    {
#if 0
        StructuredBuffer<float4> b;

        float2 size;

        float4 main(float4 position : SV_POSITION) : SV_Target
        {
            float2 p;
            int2 coords;
            p.x = position.x / 640.0f;
            p.y = position.y / 480.0f;
            coords = int2(p.x * size.x, p.y * size.y);
            return b[coords.y * size.x + coords.x];
        }
#endif
        0x43425844, 0x246caabb, 0xf1e7d6b9, 0xcbe720dc, 0xcdc23036, 0x00000001, 0x000001c0, 0x00000004,
        0x00000030, 0x00000064, 0x00000098, 0x000001b0, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008,
        0x00000020, 0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x505f5653, 0x5449534f,
        0x004e4f49, 0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000,
        0x00000003, 0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000110,
        0x00000040, 0x00000044, 0x0100486a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x040000a2,
        0x00107000, 0x00000000, 0x00000010, 0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03000065,
        0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x08000038, 0x00100032, 0x00000000, 0x00101516,
        0x00000000, 0x00208516, 0x00000000, 0x00000000, 0x0a000038, 0x00100032, 0x00000000, 0x00100046,
        0x00000000, 0x00004002, 0x3b088889, 0x3acccccd, 0x00000000, 0x00000000, 0x05000043, 0x00100032,
        0x00000000, 0x00100046, 0x00000000, 0x0a000032, 0x00100012, 0x00000000, 0x0010000a, 0x00000000,
        0x0020800a, 0x00000000, 0x00000000, 0x0010001a, 0x00000000, 0x0500001c, 0x00100012, 0x00000000,
        0x0010000a, 0x00000000, 0x090000a7, 0x001020f2, 0x00000000, 0x0010000a, 0x00000000, 0x00004001,
        0x00000000, 0x00107e46, 0x00000000, 0x0100003e, 0x30494653, 0x00000008, 0x00000002, 0x00000000,
    };
    static const D3D12_SHADER_BYTECODE ps_structured = {ps_structured_code, sizeof(ps_structured_code)};
    static const unsigned int rgba16[] =
    {
        0xff0000ff, 0xff00ffff, 0xff00ff00, 0xffffff00,
        0xffff0000, 0xffff00ff, 0xff000000, 0xff7f7f7f,
        0xffffffff, 0xffffffff, 0xffffffff, 0xff000000,
        0xffffffff, 0xff000000, 0xff000000, 0xff000000,
    };
    static const unsigned int rgba4[] =
    {
        0xffffffff, 0xff0000ff,
        0xff000000, 0xff00ff00,
    };
    static const BYTE r4[] =
    {
        0xde, 0xad,
        0xba, 0xbe,
    };
    static const struct vec4 rgba_float[] =
    {
        {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f},
    };
    static const struct buffer rgba16_buffer = {sizeof(rgba16), 0, &rgba16};
    static const struct buffer rgba16_offset_buffer = {256 + sizeof(rgba16), 256, &rgba16};
    static const struct buffer rgba4_buffer  = {sizeof(rgba4), 0, &rgba4};
    static const struct buffer r4_buffer = {sizeof(r4), 0, &r4};
    static const struct buffer r4_offset_buffer = {256 + sizeof(r4), 256, &r4};
    static const struct buffer float_buffer = {sizeof(rgba_float), 0, &rgba_float, sizeof(*rgba_float)};
    static const struct buffer float_offset_buffer = {256 + sizeof(rgba_float), 256,
            &rgba_float, sizeof(*rgba_float)};
    static const unsigned int rgba16_colors2x2[] =
    {
        0xff0000ff, 0xff0000ff, 0xff00ffff, 0xff00ffff,
        0xff0000ff, 0xff0000ff, 0xff00ffff, 0xff00ffff,
        0xff00ff00, 0xff00ff00, 0xffffff00, 0xffffff00,
        0xff00ff00, 0xff00ff00, 0xffffff00, 0xffffff00,
    };
    static const unsigned int rgba16_colors1x1[] =
    {
        0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff,
        0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff,
        0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff,
        0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff,
    };
    static const unsigned int rgba4_colors[] =
    {
        0xffffffff, 0xffffffff, 0xff0000ff, 0xff0000ff,
        0xffffffff, 0xffffffff, 0xff0000ff, 0xff0000ff,
        0xff000000, 0xff000000, 0xff00ff00, 0xff00ff00,
        0xff000000, 0xff000000, 0xff00ff00, 0xff00ff00,
    };
    static const unsigned int r4_colors[] =
    {
        0xff0000de, 0xff0000de, 0xff0000ad, 0xff0000ad,
        0xff0000de, 0xff0000de, 0xff0000ad, 0xff0000ad,
        0xff0000ba, 0xff0000ba, 0xff0000be, 0xff0000be,
        0xff0000ba, 0xff0000ba, 0xff0000be, 0xff0000be,
    };
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};

    static const struct test
    {
        const D3D12_SHADER_BYTECODE *shader;
        const struct buffer *buffer;
        DXGI_FORMAT srv_format;
        unsigned int srv_first_element;
        unsigned int srv_element_count;
        struct vec2 size;
        const unsigned int *expected_colors;
    }
    tests[] =
    {
        {&ps_float4,     &rgba16_buffer,        DXGI_FORMAT_R8G8B8A8_UNORM,   0, 16, {4.0f, 4.0f}, rgba16},
        {&ps_float4,     &rgba16_offset_buffer, DXGI_FORMAT_R8G8B8A8_UNORM,  64, 16, {4.0f, 4.0f}, rgba16},
        {&ps_float4,     &rgba16_buffer,        DXGI_FORMAT_R8G8B8A8_UNORM,   0,  4, {2.0f, 2.0f}, rgba16_colors2x2},
        {&ps_float4,     &rgba16_buffer,        DXGI_FORMAT_R8G8B8A8_UNORM,   0,  1, {1.0f, 1.0f}, rgba16_colors1x1},
        {&ps_float4,     &rgba4_buffer,         DXGI_FORMAT_R8G8B8A8_UNORM,   0,  4, {2.0f, 2.0f}, rgba4_colors},
        {&ps_float4,     &r4_buffer,            DXGI_FORMAT_R8_UNORM,         0,  4, {2.0f, 2.0f}, r4_colors},
        {&ps_float4,     &r4_offset_buffer,     DXGI_FORMAT_R8_UNORM,       256,  4, {2.0f, 2.0f}, r4_colors},
        {&ps_structured, &float_buffer,         DXGI_FORMAT_UNKNOWN,          0,  4, {2.0f, 2.0f}, rgba4_colors},
        {&ps_structured, &float_offset_buffer,  DXGI_FORMAT_UNKNOWN,         16,  4, {2.0f, 2.0f}, rgba4_colors},
    };

    memset(&desc, 0, sizeof(desc));
    desc.rt_width = 640;
    desc.rt_height = 480;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 0;
    root_parameters[1].Constants.RegisterSpace = 0;
    root_parameters[1].Constants.Num32BitValues = 2;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&descriptor_heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);
    cpu_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(descriptor_heap);
    gpu_handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(descriptor_heap);

    buffer = NULL;
    current_shader = NULL;
    current_buffer = NULL;
    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        const struct test *test = &tests[i];

        if (current_shader != test->shader)
        {
            if (context.pipeline_state)
                ID3D12PipelineState_Release(context.pipeline_state);
            current_shader = tests[i].shader;
            context.pipeline_state = create_pipeline_state(context.device,
                    context.root_signature, context.render_target_desc.Format,
                    NULL, current_shader, NULL);
        }

        if (current_buffer != test->buffer)
        {
            if (buffer)
                ID3D12Resource_Release(buffer);

            current_buffer = test->buffer;

            buffer = create_default_buffer(device, current_buffer->byte_count,
                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

            upload_buffer_data(buffer, current_buffer->data_offset,
                    current_buffer->byte_count - current_buffer->data_offset,
                    current_buffer->data, queue, command_list);
            reset_command_list(command_list, context.allocator);

            transition_sub_resource_state(command_list, buffer, 0,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        memset(&srv_desc, 0, sizeof(srv_desc));
        srv_desc.Format = test->srv_format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Buffer.FirstElement = test->srv_first_element;
        srv_desc.Buffer.NumElements = test->srv_element_count;
        srv_desc.Buffer.StructureByteStride = current_buffer->structure_byte_stride;
        ID3D12Device_CreateShaderResourceView(device, buffer, &srv_desc, cpu_handle);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &descriptor_heap);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gpu_handle);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 2, &test->size.x, 0);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        transition_sub_resource_state(command_list, context.render_target, 0,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
        for (y = 0; y < 4; ++y)
        {
            for (x = 0; x < 4; ++x)
            {
                color = get_readback_uint(&rb, 80 + x * 160, 60 + y * 120);
                expected_color = test->expected_colors[y * 4 + x];
                ok(compare_color(color, expected_color, 1),
                        "Test %u: Got 0x%08x, expected 0x%08x at (%u, %u).\n",
                        i, color, expected_color, x, y);
            }
        }
        release_resource_readback(&rb);

        reset_command_list(command_list, context.allocator);
        transition_sub_resource_state(command_list, context.render_target, 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    ID3D12DescriptorHeap_Release(descriptor_heap);
    ID3D12Resource_Release(buffer);
    destroy_test_context(&context);
}

static void test_create_query_heap(void)
{
    ID3D12Device *device;
    D3D12_QUERY_HEAP_DESC heap_desc;
    ID3D12QueryHeap *query_heap;
    ULONG refcount;
    HRESULT hr;
    int i;

    static const D3D12_QUERY_HEAP_TYPE types[] =
    {
        D3D12_QUERY_HEAP_TYPE_OCCLUSION,
        D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
        D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,
    };

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(types); ++i)
    {
        heap_desc.Type = types[i];
        heap_desc.Count = 1;
        heap_desc.NodeMask = 0;

        hr = ID3D12Device_CreateQueryHeap(device, &heap_desc, &IID_ID3D12QueryHeap, (void **)&query_heap);
        ok(hr == S_OK, "ID3D12Device_CreateQueryHeap failed, type %u, hr %#x.\n", types[i], hr);

        ID3D12QueryHeap_Release(query_heap);
    }

    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
    heap_desc.Count = 1;
    heap_desc.NodeMask = 0;

    hr = ID3D12Device_CreateQueryHeap(device, &heap_desc, &IID_ID3D12QueryHeap, (void **)&query_heap);
    todo(hr == S_OK, "ID3D12Device_CreateQueryHeap failed, type %u, hr %#x.\n", heap_desc.Type, hr);

    if (hr == S_OK)
        ID3D12QueryHeap_Release(query_heap);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_query_timestamp(void)
{
    ID3D12GraphicsCommandList *command_list;
    struct test_context_desc desc;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    D3D12_QUERY_HEAP_DESC heap_desc;
    ID3D12QueryHeap *query_heap;
    ID3D12Resource *resource;
    struct resource_readback rb;
    UINT64 timestamps[4], timestamp_frequency, timestamp_diff, time_diff;
    time_t time_start, time_end;
    HRESULT hr;
    int i;

    time_start = time(NULL);

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    hr = ID3D12CommandQueue_GetTimestampFrequency(queue, &timestamp_frequency);
    ok(SUCCEEDED(hr), "ID3D12CommandQueue_GetTimestampFrequency failed, hr %#x.\n", hr);

    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap_desc.Count = ARRAY_SIZE(timestamps);
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateQueryHeap(device, &heap_desc, &IID_ID3D12QueryHeap, (void **)&query_heap);
    ok(SUCCEEDED(hr), "ID3D12Device_CreateQueryHeap failed, type %u, hr %#x.\n", heap_desc.Type, hr);

    resource = create_readback_buffer(device, sizeof(timestamps));

    for (i = 0; i < ARRAY_SIZE(timestamps); ++i)
    {
        ID3D12GraphicsCommandList_EndQuery(command_list, query_heap, D3D12_QUERY_TYPE_TIMESTAMP, i);
        ID3D12GraphicsCommandList_ResolveQueryData(command_list, query_heap, D3D12_QUERY_TYPE_TIMESTAMP, i, 1,
                resource, i * sizeof(UINT64));
    }

    get_buffer_readback_with_command_list(resource, DXGI_FORMAT_UNKNOWN, &rb, queue, command_list);

    time_end = time(NULL) + 1;

    for (i = 0; i < ARRAY_SIZE(timestamps); ++i)
        timestamps[i] = get_readback_uint64(&rb, i, 0);

    for (i = 0; i < ARRAY_SIZE(timestamps) - 1; ++i)
        ok(timestamps[i] <= timestamps[i + 1], "Expected timestamps to monotonically increase, "
                "but got %"PRIu64" > %"PRIu64".\n", timestamps[i], timestamps[i + 1]);

    time_diff = (UINT64)difftime(time_end, time_start) * timestamp_frequency;
    timestamp_diff = timestamps[ARRAY_SIZE(timestamps) - 1] - timestamps[0];

    ok(timestamp_diff <= time_diff, "Expected timestamp difference to be bounded by CPU time difference, "
            "but got  %"PRIu64" > %"PRIu64".\n", timestamp_diff, time_diff);

    release_resource_readback(&rb);
    ID3D12QueryHeap_Release(query_heap);
    ID3D12Resource_Release(resource);
    destroy_test_context(&context);
}

static void test_query_pipeline_statistics(void)
{
    D3D12_QUERY_DATA_PIPELINE_STATISTICS *pipeline_statistics;
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    D3D12_QUERY_HEAP_DESC heap_desc;
    ID3D12QueryHeap *query_heap;
    ID3D12Resource *resource;
    struct resource_readback rb;
    unsigned int pixel_count, i;
    HRESULT hr;

    if (!init_test_context(&context, NULL))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    heap_desc.Count = 2;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateQueryHeap(device, &heap_desc, &IID_ID3D12QueryHeap, (void **)&query_heap);
    ok(SUCCEEDED(hr), "ID3D12Device_CreateQueryHeap failed, type %u, hr %#x.\n", heap_desc.Type, hr);

    resource = create_readback_buffer(device, 2 * sizeof(struct D3D12_QUERY_DATA_PIPELINE_STATISTICS));

    /* First query: do nothing. */
    ID3D12GraphicsCommandList_BeginQuery(command_list, query_heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    ID3D12GraphicsCommandList_EndQuery(command_list, query_heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    ID3D12GraphicsCommandList_ResolveQueryData(command_list, query_heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1,
            resource, 0);

    /* Second query: draw something simple. */
    ID3D12GraphicsCommandList_BeginQuery(command_list, query_heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    ID3D12GraphicsCommandList_EndQuery(command_list, query_heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
    ID3D12GraphicsCommandList_ResolveQueryData(command_list, query_heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1, 1,
            resource, sizeof(struct D3D12_QUERY_DATA_PIPELINE_STATISTICS));

    get_buffer_readback_with_command_list(resource, DXGI_FORMAT_UNKNOWN, &rb, queue, command_list);

    for (i = 0; i < sizeof(struct D3D12_QUERY_DATA_PIPELINE_STATISTICS) / sizeof(UINT64); ++i)
    {
        UINT64 value = get_readback_uint64(&rb, i, 0);
        ok(!value, "Element %d: Got %"PRIu64", expected 0.\n", i, value);
    }

    pipeline_statistics = get_readback_data(&rb, 1, 0, sizeof(*pipeline_statistics));

    /* We read 3 vertices that formed one primitive. */
    ok(pipeline_statistics->IAVertices == 3, "IAVertices: Got %"PRIu64", expected 3.\n",
            pipeline_statistics->IAVertices);
    ok(pipeline_statistics->IAPrimitives == 1, "IAPrimitives: Got %"PRIu64", expected 1.\n",
            pipeline_statistics->IAPrimitives);
    ok(pipeline_statistics->VSInvocations == 3, "VSInvocations: Got %"PRIu64", expected 3.\n",
            pipeline_statistics->VSInvocations);

    /* No geometry shader output primitives.
     * Depending on the graphics card, the geometry shader might still have been invoked, so
     * GSInvocations might be whatever. */
    ok(pipeline_statistics->GSPrimitives == 0, "GSPrimitives: Got %"PRIu64", expected 0.\n",
            pipeline_statistics->GSPrimitives);

    /* One primitive sent to the rasterizer, but it might have been broken up into smaller pieces then. */
    ok(pipeline_statistics->CInvocations == 1, "CInvocations: Got %"PRIu64", expected 1.\n",
            pipeline_statistics->CInvocations);
    ok(pipeline_statistics->CPrimitives > 0, "CPrimitives: Got %"PRIu64", expected > 0.\n",
            pipeline_statistics->CPrimitives);

    /* Exact number of pixel shader invocations depends on the graphics card. */
    pixel_count = context.render_target_desc.Width * context.render_target_desc.Height;
    ok(pipeline_statistics->PSInvocations >= pixel_count, "PSInvocations: Got %"PRIu64", expected >= %u.\n",
            pipeline_statistics->PSInvocations, pixel_count);

    /* We used no tessellation or compute shaders at all. */
    ok(pipeline_statistics->HSInvocations == 0, "HSInvocations: Got %"PRIu64", expected 0.\n",
            pipeline_statistics->HSInvocations);
    ok(pipeline_statistics->DSInvocations == 0, "DSInvocations: Got %"PRIu64", expected 0.\n",
            pipeline_statistics->DSInvocations);
    ok(pipeline_statistics->CSInvocations == 0, "CSInvocations: Got %"PRIu64", expected 0.\n",
            pipeline_statistics->CSInvocations);

    release_resource_readback(&rb);
    ID3D12QueryHeap_Release(query_heap);
    ID3D12Resource_Release(resource);
    destroy_test_context(&context);
}

static void test_query_occlusion(void)
{
    struct test_context_desc desc;
    ID3D12GraphicsCommandList *command_list;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    struct depth_stencil_resource ds;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    D3D12_QUERY_HEAP_DESC heap_desc;
    ID3D12QueryHeap *query_heap;
    ID3D12Resource *resource;
    struct resource_readback rb;
    unsigned int i;
    HRESULT hr;

    static const DWORD ps_code[] =
    {
#if 0
        float depth;

        float main() : SV_Depth
        {
            return depth;
        }
#endif
        0x43425844, 0x91af6cd0, 0x7e884502, 0xcede4f54, 0x6f2c9326, 0x00000001, 0x000000b0, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0xffffffff,
        0x00000e01, 0x445f5653, 0x68747065, 0xababab00, 0x52444853, 0x00000038, 0x00000040, 0x0000000e,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x02000065, 0x0000c001, 0x05000036, 0x0000c001,
        0x0020800a, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const struct
    {
        D3D12_QUERY_TYPE type;
        bool draw;
        float clear_depth;
        float depth;
    }
    tests[] =
    {
        {D3D12_QUERY_TYPE_OCCLUSION,        false, 1.0f, 0.5f},
        {D3D12_QUERY_TYPE_OCCLUSION,        true,  1.0f, 0.5f},
        {D3D12_QUERY_TYPE_BINARY_OCCLUSION, false, 1.0f, 0.5f},
        {D3D12_QUERY_TYPE_BINARY_OCCLUSION, true,  1.0f, 0.5f},
        {D3D12_QUERY_TYPE_OCCLUSION,        false, 0.0f, 0.5f},
        {D3D12_QUERY_TYPE_OCCLUSION,        true,  0.0f, 0.5f},
        {D3D12_QUERY_TYPE_BINARY_OCCLUSION, false, 0.0f, 0.5f},
        {D3D12_QUERY_TYPE_BINARY_OCCLUSION, true,  0.0f, 0.5f},
    };

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    init_depth_stencil(&ds, context.device, 640, 480, DXGI_FORMAT_D32_FLOAT, 0, NULL);
    set_viewport(&context.viewport, 0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    set_rect(&context.scissor_rect, 0, 0, 640, 480);

    context.root_signature = create_32bit_constants_root_signature(context.device,
            0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    init_pipeline_state_desc(&pso_desc, context.root_signature, 0, NULL, &ps, NULL);
    pso_desc.NumRenderTargets = 0;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = ID3D12Device_CreateGraphicsPipelineState(context.device, &pso_desc,
            &IID_ID3D12PipelineState, (void **)&context.pipeline_state);
    ok(SUCCEEDED(hr), "Failed to create graphics pipeline state, hr %#x.\n", hr);

    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    heap_desc.Count = ARRAY_SIZE(tests);
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateQueryHeap(device, &heap_desc, &IID_ID3D12QueryHeap, (void **)&query_heap);
    ok(SUCCEEDED(hr), "ID3D12Device_CreateQueryHeap failed, type %u, hr %#x.\n", heap_desc.Type, hr);

    resource = create_readback_buffer(device, ARRAY_SIZE(tests) * sizeof(UINT64));

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 0, NULL, FALSE, &ds.dsv_handle);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, ds.dsv_handle,
                D3D12_CLEAR_FLAG_DEPTH, tests[i].clear_depth, 0, 0, NULL);

        ID3D12GraphicsCommandList_BeginQuery(command_list, query_heap, tests[i].type, i);

        if (tests[i].draw)
        {
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 1, &tests[i].depth, 0);
            ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);
        }

        ID3D12GraphicsCommandList_EndQuery(command_list, query_heap, tests[i].type, i);
        ID3D12GraphicsCommandList_ResolveQueryData(command_list, query_heap, tests[i].type, i, 1,
                resource, i * sizeof(UINT64));
    }

    get_buffer_readback_with_command_list(resource, DXGI_FORMAT_UNKNOWN, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        const bool samples_passed = tests[i].draw && tests[i].clear_depth > tests[i].depth;
        const UINT64 result = get_readback_uint64(&rb, i, 0);
        UINT64 expected_result;

        if (tests[i].type == D3D12_QUERY_TYPE_BINARY_OCCLUSION)
            expected_result = samples_passed ? 1 : 0;
        else
            expected_result = samples_passed ? 640 * 480 : 0;

        ok(result == expected_result || (expected_result && result >= expected_result),
                "Test %u: Got unexpected result %"PRIu64".\n", i, result);
    }
    release_resource_readback(&rb);

    ID3D12QueryHeap_Release(query_heap);
    ID3D12Resource_Release(resource);
    destroy_depth_stencil(&ds);
    destroy_test_context(&context);
}

static void test_resolve_non_issued_query_data(void)
{
    static const UINT64 initial_data[] = {0xdeadbeef, 0xdeadbeef, 0xdeadbabe, 0xdeadbeef};
    ID3D12Resource *readback_buffer, *upload_buffer;
    ID3D12GraphicsCommandList *command_list;
    D3D12_QUERY_HEAP_DESC heap_desc;
    struct test_context_desc desc;
    ID3D12QueryHeap *query_heap;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    UINT64 *timestamps;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap_desc.Count = ARRAY_SIZE(initial_data);
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateQueryHeap(device, &heap_desc, &IID_ID3D12QueryHeap, (void **)&query_heap);
    ok(SUCCEEDED(hr), "Failed to create query heap, hr %#x.\n", hr);

    readback_buffer = create_readback_buffer(device, sizeof(initial_data));
    upload_buffer = create_upload_buffer(context.device, sizeof(initial_data), initial_data);

    ID3D12GraphicsCommandList_EndQuery(command_list, query_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    ID3D12GraphicsCommandList_CopyResource(command_list, readback_buffer, upload_buffer);
    ID3D12GraphicsCommandList_EndQuery(command_list, query_heap, D3D12_QUERY_TYPE_TIMESTAMP, 3);
    ID3D12GraphicsCommandList_ResolveQueryData(command_list, query_heap,
            D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, readback_buffer, 0);

    get_buffer_readback_with_command_list(readback_buffer, DXGI_FORMAT_UNKNOWN, &rb, queue, command_list);
    timestamps = get_readback_data(&rb, 0, 0, sizeof(*timestamps));
    ok(timestamps[0] != initial_data[0] && timestamps[0] > 0,
            "Got unexpected timestamp %#"PRIx64".\n", timestamps[0]);
    ok(!timestamps[1], "Got unexpected timestamp %#"PRIx64".\n", timestamps[1]);
    ok(!timestamps[2], "Got unexpected timestamp %#"PRIx64".\n", timestamps[2]);
    ok(timestamps[3] != initial_data[3] && timestamps[3] > 0,
            "Got unexpected timestamp %#"PRIx64".\n", timestamps[3]);
    release_resource_readback(&rb);

    ID3D12QueryHeap_Release(query_heap);
    ID3D12Resource_Release(readback_buffer);
    ID3D12Resource_Release(upload_buffer);
    destroy_test_context(&context);
}

static void test_execute_indirect(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc;
    D3D12_INDIRECT_ARGUMENT_DESC argument_desc;
    ID3D12CommandSignature *command_signature;
    ID3D12GraphicsCommandList *command_list;
    ID3D12Resource *argument_buffer, *uav;
    D3D12_ROOT_PARAMETER root_parameter;
    ID3D12PipelineState *pipeline_state;
    ID3D12RootSignature *root_signature;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        RWByteAddressBuffer o;

        [numthreads(1, 1, 1)]
        void main(uint3 group_id : SV_groupID)
        {
            uint idx = group_id.x + group_id.y * 2 + group_id.z * 6;
            o.Store(idx * 4, idx);
        }
#endif
        0x43425844, 0xfdd6a339, 0xf3b8096e, 0xb5977014, 0xcdb26cfd, 0x00000001, 0x00000118, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x000000c4, 0x00050050, 0x00000031, 0x0100086a,
        0x0300009d, 0x0011e000, 0x00000000, 0x0200005f, 0x00021072, 0x02000068, 0x00000001, 0x0400009b,
        0x00000001, 0x00000001, 0x00000001, 0x06000029, 0x00100012, 0x00000000, 0x0002101a, 0x00004001,
        0x00000001, 0x0600001e, 0x00100012, 0x00000000, 0x0010000a, 0x00000000, 0x0002100a, 0x08000023,
        0x00100012, 0x00000000, 0x0002102a, 0x00004001, 0x00000006, 0x0010000a, 0x00000000, 0x07000029,
        0x00100022, 0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x00000002, 0x070000a6, 0x0011e012,
        0x00000000, 0x0010001a, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
    };
    static const struct argument_data
    {
        D3D12_DRAW_ARGUMENTS draw;
        D3D12_DISPATCH_ARGUMENTS dispatch;
    }
    argument_data =
    {
        {3, 1, 0, 0},
        {2, 3, 4},
    };
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};

    if (!init_test_context(&context, NULL))
        return;
    command_list = context.list;
    queue = context.queue;

    argument_buffer = create_upload_buffer(context.device, sizeof(argument_data), &argument_data);

    argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    signature_desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &argument_desc;
    signature_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandSignature(context.device, &signature_desc,
            NULL, &IID_ID3D12CommandSignature, (void **)&command_signature);
    ok(SUCCEEDED(hr), "Failed to create command signature, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_ExecuteIndirect(command_list, command_signature, 1, argument_buffer, 0, NULL, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target, 0, queue, command_list, 0xff00ff00, 0);

    reset_command_list(command_list, context.allocator);

    ID3D12CommandSignature_Release(command_signature);
    argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    signature_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    hr = ID3D12Device_CreateCommandSignature(context.device, &signature_desc,
            NULL, &IID_ID3D12CommandSignature, (void **)&command_signature);
    ok(SUCCEEDED(hr), "Failed to create command signature, hr %#x.\n", hr);

    uav = create_default_buffer(context.device, 2 * 3 * 4 * sizeof(UINT),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameter.Descriptor.ShaderRegister = 0;
    root_parameter.Descriptor.RegisterSpace = 0;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = &root_parameter;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(context.device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, root_signature);
    pipeline_state = create_compute_pipeline_state(context.device, root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));
    ID3D12GraphicsCommandList_SetPipelineState(command_list, pipeline_state);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(command_list,
            0, ID3D12Resource_GetGPUVirtualAddress(uav));
    ID3D12GraphicsCommandList_ExecuteIndirect(command_list, command_signature, 1, argument_buffer,
            offsetof(struct argument_data, dispatch), NULL, 0);

    transition_sub_resource_state(command_list, uav, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(uav, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    for (i = 0; i < rb.width; ++i)
    {
        unsigned int ret = get_readback_uint(&rb, i, 0);
        ok(ret == i, "Got unexpected result %#x at index %u.\n", ret, i);
    }
    release_resource_readback(&rb);

    ID3D12PipelineState_Release(pipeline_state);
    ID3D12RootSignature_Release(root_signature);
    ID3D12Resource_Release(uav);
    ID3D12CommandSignature_Release(command_signature);
    ID3D12Resource_Release(argument_buffer);
    destroy_test_context(&context);
}

static void test_dispatch_zero_thread_groups(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc;
    D3D12_INDIRECT_ARGUMENT_DESC argument_desc;
    ID3D12CommandSignature *command_signature;
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[2];
    ID3D12Resource *argument_buffer, *uav;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    unsigned int ret, i;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        RWByteAddressBuffer o;

        uint v;

        [numthreads(1, 1, 1)]
        void main()
        {
            o.Store(0, v);
        }
#endif
        0x43425844, 0x3ad946e3, 0x83e33b81, 0x83532aa4, 0x40831f89, 0x00000001, 0x000000b0, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x0000005c, 0x00050050, 0x00000017, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x0300009d, 0x0011e000, 0x00000000, 0x0400009b,
        0x00000001, 0x00000001, 0x00000001, 0x080000a6, 0x0011e012, 0x00000000, 0x00004001, 0x00000000,
        0x0020800a, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_DISPATCH_ARGUMENTS argument_data[] =
    {
        {1, 1, 1},
        {0, 3, 4},
        {0, 0, 4},
        {0, 0, 0},
        {4, 0, 0},
        {4, 0, 3},
        {4, 2, 0},
        {0, 2, 0},
        {0, 0, 0},
    };

    if (!init_compute_test_context(&context))
        return;
    command_list = context.list;
    queue = context.queue;

    argument_buffer = create_upload_buffer(context.device, sizeof(argument_data), &argument_data);

    argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    signature_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &argument_desc;
    signature_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandSignature(context.device, &signature_desc,
            NULL, &IID_ID3D12CommandSignature, (void **)&command_signature);
    ok(SUCCEEDED(hr), "Failed to create command signature, hr %#x.\n", hr);

    uav = create_default_buffer(context.device, 2 * 256, /* minTexelBufferOffsetAlignment */
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[0].Descriptor.RegisterSpace = 0;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 0;
    root_parameters[1].Constants.RegisterSpace = 0;
    root_parameters[1].Constants.Num32BitValues = 1;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 2;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(context.device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    context.pipeline_state = create_compute_pipeline_state(context.device, context.root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);

    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(command_list,
            0, ID3D12Resource_GetGPUVirtualAddress(uav));
    for (i = 0; i < ARRAY_SIZE(argument_data); ++i)
    {
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(command_list,
                1, 10 + i, 0);
        ID3D12GraphicsCommandList_ExecuteIndirect(command_list, command_signature,
                1, argument_buffer, i * sizeof(*argument_data), NULL, 0);
    }

    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(command_list,
            0, ID3D12Resource_GetGPUVirtualAddress(uav) + 256);
    for (i = 0; i < ARRAY_SIZE(argument_data); ++i)
    {
        const D3D12_DISPATCH_ARGUMENTS *arg = &argument_data[i];
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(command_list,
                1, 50 + i, 0);
        ID3D12GraphicsCommandList_Dispatch(command_list,
                arg->ThreadGroupCountX, arg->ThreadGroupCountY, arg->ThreadGroupCountZ);
    }

    transition_sub_resource_state(command_list, uav, 0,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(uav, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    ret = get_readback_uint(&rb, 0, 0);
    ok(ret == 10, "Got unexpected result %#x.\n", ret);
    ret = get_readback_uint(&rb, 64, 0);
    ok(ret == 50, "Got unexpected result %#x.\n", ret);
    release_resource_readback(&rb);

    ID3D12Resource_Release(uav);
    ID3D12CommandSignature_Release(command_signature);
    ID3D12Resource_Release(argument_buffer);
    destroy_test_context(&context);
}

static void test_instance_id(void)
{
    D3D12_COMMAND_SIGNATURE_DESC signature_desc;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    D3D12_INDIRECT_ARGUMENT_DESC argument_desc;
    ID3D12CommandSignature *command_signature;
    ID3D12GraphicsCommandList *command_list;
    D3D12_INPUT_LAYOUT_DESC input_layout;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2];
    D3D12_VERTEX_BUFFER_VIEW vbv[2];
    ID3D12Resource *argument_buffer;
    ID3D12Resource *render_target;
    struct test_context_desc desc;
    struct test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    ID3D12Resource *vb[2];
    unsigned int i;
    HRESULT hr;

    static const D3D12_INPUT_ELEMENT_DESC layout_desc[] =
    {
        {"position", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"color",    0, DXGI_FORMAT_R8_UNORM,           1, D3D12_APPEND_ALIGNED_ELEMENT,
                D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"v_offset", 0, DXGI_FORMAT_R32_FLOAT,          1, D3D12_APPEND_ALIGNED_ELEMENT,
                D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    static const DWORD vs_code[] =
    {
#if 0
        struct vs_in
        {
            float4 position : Position;
            float color : Color;
            float v_offset : V_Offset;
            uint instance_id : SV_InstanceId;
        };

        struct vs_out
        {
            float4 position : SV_Position;
            float color : Color;
            uint instance_id : InstanceId;
        };

        void main(vs_in i, out vs_out o)
        {
            o.position = i.position;
            o.position.x += i.v_offset;
            o.color = i.color;
            o.instance_id = i.instance_id;
        }
#endif
        0x43425844, 0xcde3cfbf, 0xe2e3d090, 0xe2eb1038, 0x7e5ad1cf, 0x00000001, 0x00000204, 0x00000003,
        0x0000002c, 0x000000c4, 0x0000013c, 0x4e475349, 0x00000090, 0x00000004, 0x00000008, 0x00000068,
        0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x00000f0f, 0x00000071, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000101, 0x00000077, 0x00000000, 0x00000000, 0x00000003, 0x00000002,
        0x00000101, 0x00000080, 0x00000000, 0x00000008, 0x00000001, 0x00000003, 0x00000101, 0x69736f50,
        0x6e6f6974, 0x6c6f4300, 0x5600726f, 0x66664f5f, 0x00746573, 0x495f5653, 0x6174736e, 0x4965636e,
        0xabab0064, 0x4e47534f, 0x00000070, 0x00000003, 0x00000008, 0x00000050, 0x00000000, 0x00000001,
        0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000, 0x00000000, 0x00000003, 0x00000001,
        0x00000e01, 0x00000062, 0x00000000, 0x00000000, 0x00000001, 0x00000002, 0x00000e01, 0x505f5653,
        0x7469736f, 0x006e6f69, 0x6f6c6f43, 0x6e490072, 0x6e617473, 0x64496563, 0xababab00, 0x52444853,
        0x000000c0, 0x00010040, 0x00000030, 0x0300005f, 0x001010f2, 0x00000000, 0x0300005f, 0x00101012,
        0x00000001, 0x0300005f, 0x00101012, 0x00000002, 0x04000060, 0x00101012, 0x00000003, 0x00000008,
        0x04000067, 0x001020f2, 0x00000000, 0x00000001, 0x03000065, 0x00102012, 0x00000001, 0x03000065,
        0x00102012, 0x00000002, 0x07000000, 0x00102012, 0x00000000, 0x0010100a, 0x00000000, 0x0010100a,
        0x00000002, 0x05000036, 0x001020e2, 0x00000000, 0x00101e56, 0x00000000, 0x05000036, 0x00102012,
        0x00000001, 0x0010100a, 0x00000001, 0x05000036, 0x00102012, 0x00000002, 0x0010100a, 0x00000003,
        0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs = {vs_code, sizeof(vs_code)};
    static const DWORD ps_code[] =
    {
#if 0
        struct vs_out
        {
            float4 position : SV_Position;
            float color : Color;
            uint instance_id : InstanceId;
        };

        void main(vs_out i, out float4 o0 : SV_Target0, out uint4 o1 : SV_Target1)
        {
            o0 = float4(i.color, i.color, i.color, 1.0f);
            o1 = i.instance_id;
        }
#endif
        0x43425844, 0xda0ad0bb, 0x4743f5f5, 0xfbc6d0b1, 0x7c8e7df5, 0x00000001, 0x00000170, 0x00000003,
        0x0000002c, 0x000000a4, 0x000000f0, 0x4e475349, 0x00000070, 0x00000003, 0x00000008, 0x00000050,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000101, 0x00000062, 0x00000000, 0x00000000, 0x00000001, 0x00000002,
        0x00000101, 0x505f5653, 0x7469736f, 0x006e6f69, 0x6f6c6f43, 0x6e490072, 0x6e617473, 0x64496563,
        0xababab00, 0x4e47534f, 0x00000044, 0x00000002, 0x00000008, 0x00000038, 0x00000000, 0x00000000,
        0x00000003, 0x00000000, 0x0000000f, 0x00000038, 0x00000001, 0x00000000, 0x00000001, 0x00000001,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x00000078, 0x00000040, 0x0000001e,
        0x03001062, 0x00101012, 0x00000001, 0x03000862, 0x00101012, 0x00000002, 0x03000065, 0x001020f2,
        0x00000000, 0x03000065, 0x001020f2, 0x00000001, 0x05000036, 0x00102072, 0x00000000, 0x00101006,
        0x00000001, 0x05000036, 0x00102082, 0x00000000, 0x00004001, 0x3f800000, 0x05000036, 0x001020f2,
        0x00000001, 0x00101006, 0x00000002, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const struct
    {
        struct vec4 position;
    }
    stream0[] =
    {
        {{-1.00f, 0.0f, 0.0f, 1.0f}},
        {{-1.00f, 1.0f, 0.0f, 1.0f}},
        {{-0.75f, 0.0f, 0.0f, 1.0f}},
        {{-0.75f, 1.0f, 0.0f, 1.0f}},
        /* indirect draws data */
        {{-1.00f, -1.0f, 0.0f, 1.0f}},
        {{-1.00f,  0.0f, 0.0f, 1.0f}},
        {{-0.75f, -1.0f, 0.0f, 1.0f}},
        {{-0.75f,  0.0f, 0.0f, 1.0f}},
    };
    static const struct
    {
        BYTE color;
        float v_offset;
    }
    stream1[] =
    {
        {0xf0, 0.00f},
        {0x80, 0.25f},
        {0x10, 0.50f},
        {0x40, 0.75f},

        {0xaa, 1.00f},
        {0xbb, 1.25f},
        {0xcc, 1.50f},
        {0x90, 1.75f},
    };
    static const D3D12_DRAW_ARGUMENTS argument_data[] =
    {
        {4, 4, 4, 0},
        {4, 4, 4, 4},
    };
    static const struct
    {
        RECT rect;
        unsigned int color;
        unsigned int instance_id;
    }
    expected_results[] =
    {
        {{ 0, 0, 10, 10}, 0xfff0f0f0, 0},
        {{10, 0, 20, 10}, 0xff808080, 1},
        {{20, 0, 30, 10}, 0xff101010, 2},
        {{30, 0, 40, 10}, 0xff404040, 3},
        {{40, 0, 50, 10}, 0xffaaaaaa, 0},
        {{50, 0, 60, 10}, 0xffbbbbbb, 1},
        {{60, 0, 70, 10}, 0xffcccccc, 2},
        {{70, 0, 80, 10}, 0xff909090, 3},
        /* indirect draws results */
        {{ 0, 10, 10, 20}, 0xfff0f0f0, 0},
        {{10, 10, 20, 20}, 0xff808080, 1},
        {{20, 10, 30, 20}, 0xff101010, 2},
        {{30, 10, 40, 20}, 0xff404040, 3},
        {{40, 10, 50, 20}, 0xffaaaaaa, 0},
        {{50, 10, 60, 20}, 0xffbbbbbb, 1},
        {{60, 10, 70, 20}, 0xffcccccc, 2},
        {{70, 10, 80, 20}, 0xff909090, 3},
    };
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};

    memset(&desc, 0, sizeof(desc));
    desc.rt_width = 80;
    desc.rt_height = 20;
    desc.rt_descriptor_count = 2;
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    rtvs[0] = context.rtv;
    rtvs[1] = get_cpu_rtv_handle(&context, context.rtv_heap, 1);

    desc.rt_format = DXGI_FORMAT_R32_UINT;
    create_render_target(&context, &desc, &render_target, &rtvs[1]);

    context.root_signature = create_empty_root_signature(context.device,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    input_layout.pInputElementDescs = layout_desc;
    input_layout.NumElements = ARRAY_SIZE(layout_desc);
    init_pipeline_state_desc(&pso_desc, context.root_signature, 0, &vs, &ps, &input_layout);
    pso_desc.NumRenderTargets = 2;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.RTVFormats[1] = DXGI_FORMAT_R32_UINT;
    hr = ID3D12Device_CreateGraphicsPipelineState(context.device, &pso_desc,
            &IID_ID3D12PipelineState, (void **)&context.pipeline_state);
    ok(SUCCEEDED(hr), "Failed to create graphics pipeline state, hr %#x.\n", hr);

    vb[0] = create_upload_buffer(context.device, sizeof(stream0), stream0);
    vbv[0].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb[0]);
    vbv[0].StrideInBytes = sizeof(*stream0);
    vbv[0].SizeInBytes = sizeof(stream0);

    vb[1] = create_upload_buffer(context.device, sizeof(stream1), stream1);
    vbv[1].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb[1]);
    vbv[1].StrideInBytes = sizeof(*stream1);
    vbv[1].SizeInBytes = sizeof(stream1);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtvs[0], white, 0, NULL);
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtvs[1], white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 2, rtvs, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, ARRAY_SIZE(vbv), vbv);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 4, 4, 0, 0);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 4, 4, 0, 4);

    argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    signature_desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &argument_desc;
    signature_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandSignature(context.device, &signature_desc,
            NULL, &IID_ID3D12CommandSignature, (void **)&command_signature);
    ok(SUCCEEDED(hr), "Failed to create command signature, hr %#x.\n", hr);

    argument_buffer = create_upload_buffer(context.device, sizeof(argument_data), &argument_data);
    ID3D12GraphicsCommandList_ExecuteIndirect(command_list, command_signature,
            2, argument_buffer, 0, NULL, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(expected_results); ++i)
        check_readback_data_uint(&rb, &expected_results[i].rect, expected_results[i].color, 1);
    release_resource_readback(&rb);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(render_target, 0, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(expected_results); ++i)
        check_readback_data_uint(&rb, &expected_results[i].rect, expected_results[i].instance_id, 0);
    release_resource_readback(&rb);

    ID3D12CommandSignature_Release(command_signature);
    ID3D12Resource_Release(argument_buffer);
    ID3D12Resource_Release(render_target);
    ID3D12Resource_Release(vb[0]);
    ID3D12Resource_Release(vb[1]);
    destroy_test_context(&context);
}

static void test_texture_copy_region(void)
{
    struct test_context_desc desc;
    struct test_context context;
    ID3D12Device *device;
    ID3D12GraphicsCommandList *command_list;
    ID3D12CommandQueue *queue;
    ID3D12Resource *texture_src, *texture_dst;
    D3D12_SUBRESOURCE_DATA texture_data;
    D3D12_TEXTURE_COPY_LOCATION location_src, location_dst;
    struct resource_readback rb;
    D3D12_BOX copy_box;
    unsigned int i, j;

    static const unsigned int clear_data[] =
    {
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    };
    static const unsigned int bitmap_data[] =
    {
        0xff00ff00, 0xff00ff01, 0xff00ff02, 0xff00ff03,
        0xff00ff10, 0xff00ff12, 0xff00ff12, 0xff00ff13,
        0xff00ff20, 0xff00ff21, 0xff00ff22, 0xff00ff23,
        0xff00ff30, 0xff00ff31, 0xff00ff32, 0xff00ff33,
    };
    static const unsigned int result_data[] =
    {
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0xff00ff00, 0xff00ff01, 0x00000000,
        0x00000000, 0xff00ff10, 0xff00ff12, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    };

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    texture_src = create_texture(device, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST);
    texture_data.pData = bitmap_data;
    texture_data.RowPitch = 4 * sizeof(*bitmap_data);
    texture_data.SlicePitch = texture_data.RowPitch * 4;
    upload_texture_data(texture_src, &texture_data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);

    texture_dst = create_texture(device, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST);
    texture_data.pData = clear_data;
    texture_data.RowPitch = 4 * sizeof(*bitmap_data);
    texture_data.SlicePitch = texture_data.RowPitch * 4;
    upload_texture_data(texture_dst, &texture_data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);

    transition_resource_state(command_list, texture_src,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);

    location_src.pResource = texture_src;
    location_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location_src.SubresourceIndex = 0;
    location_dst.pResource = texture_dst;
    location_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location_dst.SubresourceIndex = 0;

    copy_box.left = 0;
    copy_box.top = 0;
    copy_box.right = 2;
    copy_box.bottom = 2;
    copy_box.front = 0;
    copy_box.back = 1;

    ID3D12GraphicsCommandList_CopyTextureRegion(command_list, &location_dst, 1, 1, 0, &location_src, &copy_box);

    transition_resource_state(command_list, texture_dst,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_texture_readback_with_command_list(texture_dst, 0, &rb, queue, command_list);
    for (i = 0; i < 4; ++i)
    {
        for (j = 0; j < 4; ++j)
        {
            unsigned int color = get_readback_uint(&rb, j, i);
            unsigned int expected = result_data[i * 4 + j];

            ok(color == expected, "Got unexpected color 0x%08x at (%u, %u), expected 0x%08x\n",
                    color, j, i, expected);
        }
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(texture_src);
    ID3D12Resource_Release(texture_dst);
    destroy_test_context(&context);
}

static void test_separate_bindings(void)
{
    ID3D12Resource *cs_raw_buffer, *cs_raw_uav_buffer;
    ID3D12Resource *ps_raw_buffer, *ps_raw_uav_buffer;
    ID3D12Resource *cs_textures[2], *ps_textures[2];
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[2];
    ID3D12GraphicsCommandList *command_list;
    D3D12_ROOT_PARAMETER root_parameters[4];
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12PipelineState *compute_pso;
    ID3D12Resource *cs_cb, *ps_cb;
    struct test_context_desc desc;
    struct resource_readback rb;
    D3D12_SUBRESOURCE_DATA data;
    struct test_context context;
    ID3D12DescriptorHeap *heap;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    static const DWORD cs_code[] =
    {
#if 0
        ByteAddressBuffer t0;

        RWByteAddressBuffer u1 : register(u1);

        cbuffer cb0
        {
            float4 cb0;
        };

        Texture2D t1;
        RWTexture2D<float> u2 : register(u2);

        [numthreads(1, 1, 1)]
        void main()
        {
            uint ret = 0xffffffff;

            if (t0.Load(0) != 2)
                ret = 0;
            if (any(cb0 != float4(4, 8, 16, 32)))
                ret = 0;
            if (any(t1.Load(0) != float4(4, 8, 16, 32)))
                ret = 0;
            if (u2[(int2)0] != 4)
                ret = 0;

            u1.Store(0, ret);
        }
#endif
        0x43425844, 0x5ef0e316, 0x8a886806, 0x06279aa8, 0x10936fa5, 0x00000001, 0x000002bc, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000268, 0x00050050, 0x0000009a, 0x0100086a,
        0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x030000a1, 0x00107000, 0x00000000, 0x04001858,
        0x00107000, 0x00000001, 0x00005555, 0x0300009d, 0x0011e000, 0x00000001, 0x0400189c, 0x0011e000,
        0x00000002, 0x00005555, 0x02000068, 0x00000002, 0x0400009b, 0x00000001, 0x00000001, 0x00000001,
        0x8c00002d, 0x800000c2, 0x00155543, 0x001000f2, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00107e46, 0x00000001, 0x0a000039, 0x001000f2, 0x00000000, 0x00100e46,
        0x00000000, 0x00004002, 0x40800000, 0x41000000, 0x41800000, 0x42000000, 0x0700003c, 0x00100032,
        0x00000000, 0x00100ae6, 0x00000000, 0x00100046, 0x00000000, 0x0700003c, 0x00100012, 0x00000000,
        0x0010001a, 0x00000000, 0x0010000a, 0x00000000, 0x0b000039, 0x001000f2, 0x00000001, 0x00208e46,
        0x00000000, 0x00000000, 0x00004002, 0x40800000, 0x41000000, 0x41800000, 0x42000000, 0x0700003c,
        0x00100062, 0x00000000, 0x00100ba6, 0x00000001, 0x00100106, 0x00000001, 0x0700003c, 0x00100022,
        0x00000000, 0x0010002a, 0x00000000, 0x0010001a, 0x00000000, 0x0700003c, 0x00100012, 0x00000000,
        0x0010000a, 0x00000000, 0x0010001a, 0x00000000, 0x8c0000a3, 0x800000c2, 0x00155543, 0x00100022,
        0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0011ee16, 0x00000002,
        0x07000039, 0x00100022, 0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x40800000, 0x0700003c,
        0x00100012, 0x00000000, 0x0010001a, 0x00000000, 0x0010000a, 0x00000000, 0x890000a5, 0x800002c2,
        0x00199983, 0x00100022, 0x00000000, 0x00004001, 0x00000000, 0x00107006, 0x00000000, 0x07000020,
        0x00100022, 0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x00000002, 0x09000037, 0x00100012,
        0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x00000000, 0x0010001a, 0x00000000, 0x070000a6,
        0x0011e012, 0x00000001, 0x00004001, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
    };
    static const DWORD ps_code[] =
    {
#if 0
        ByteAddressBuffer t0;

        RWByteAddressBuffer u1 : register(u1);

        cbuffer cb0
        {
            float4 cb0;
        };

        Texture2D t1;
        RWTexture2D<float> u2 : register(u2);

        float4 main() : SV_Target0
        {
            bool ret = true;

            if (t0.Load(0) != 1)
                ret = false;
            if (u1.Load(0) != 2)
                ret = false;
            if (any(cb0 != float4(1, 2, 3, 4)))
                ret = false;
            if (any(t1.Load(0) != float4(1, 2, 3, 4)))
                ret = false;
            if (u2[(int2)0] != 1)
                ret = false;

            return ret ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
        }
#endif
        0x43425844, 0xb5db404c, 0xd1dd05ca, 0xf5c1284d, 0x58d71b13, 0x00000001, 0x00000358, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000002e0, 0x00000050, 0x000000b8,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x030000a1, 0x00107000, 0x00000000,
        0x04001858, 0x00107000, 0x00000001, 0x00005555, 0x0300009d, 0x0011e000, 0x00000001, 0x0400189c,
        0x0011e000, 0x00000002, 0x00005555, 0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000002,
        0x0b000039, 0x001000f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00004002, 0x3f800000,
        0x40000000, 0x40400000, 0x40800000, 0x0700003c, 0x00100032, 0x00000000, 0x00100ae6, 0x00000000,
        0x00100046, 0x00000000, 0x0700003c, 0x00100012, 0x00000000, 0x0010001a, 0x00000000, 0x0010000a,
        0x00000000, 0x890000a5, 0x800002c2, 0x00199983, 0x00100022, 0x00000000, 0x00004001, 0x00000000,
        0x0011e006, 0x00000001, 0x07000027, 0x00100022, 0x00000000, 0x0010001a, 0x00000000, 0x00004001,
        0x00000002, 0x0700003c, 0x00100012, 0x00000000, 0x0010000a, 0x00000000, 0x0010001a, 0x00000000,
        0x8c00002d, 0x800000c2, 0x00155543, 0x001000f2, 0x00000001, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00107e46, 0x00000001, 0x0a000039, 0x001000f2, 0x00000001, 0x00100e46,
        0x00000001, 0x00004002, 0x3f800000, 0x40000000, 0x40400000, 0x40800000, 0x0700003c, 0x00100062,
        0x00000000, 0x00100ba6, 0x00000001, 0x00100106, 0x00000001, 0x0700003c, 0x00100022, 0x00000000,
        0x0010002a, 0x00000000, 0x0010001a, 0x00000000, 0x0700003c, 0x00100012, 0x00000000, 0x0010001a,
        0x00000000, 0x0010000a, 0x00000000, 0x8c0000a3, 0x800000c2, 0x00155543, 0x00100022, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0011ee16, 0x00000002, 0x07000039,
        0x00100022, 0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x3f800000, 0x0700003c, 0x00100012,
        0x00000000, 0x0010001a, 0x00000000, 0x0010000a, 0x00000000, 0x890000a5, 0x800002c2, 0x00199983,
        0x00100022, 0x00000000, 0x00004001, 0x00000000, 0x00107006, 0x00000000, 0x07000020, 0x00100022,
        0x00000000, 0x0010001a, 0x00000000, 0x00004001, 0x00000001, 0x09000037, 0x00100012, 0x00000000,
        0x0010000a, 0x00000000, 0x00004001, 0x00000000, 0x0010001a, 0x00000000, 0x0f000037, 0x001020f2,
        0x00000000, 0x00100006, 0x00000000, 0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000,
        0x00004002, 0x3f800000, 0x00000000, 0x00000000, 0x3f800000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const struct vec4 cs_data = {4.0f, 8.0f, 16.0f, 32.0f};
    static const struct vec4 ps_data = {1.0f, 2.0f, 3.0f, 4.0f};
    static const float cs_texture_data = 4.0f;
    static const float ps_texture_data = 1.0f;
    static const uint32_t cs_raw_data = 2;
    static const uint32_t ps_raw_data = 1;
    static const uint32_t ps_raw_uav_data = 2;
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};

    memset(&desc, 0, sizeof(desc));
    desc.no_root_signature = true;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[0].Descriptor.RegisterSpace = 0;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_parameters[2].Descriptor.ShaderRegister = 1;
    root_parameters[2].Descriptor.RegisterSpace = 0;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 1;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[1].NumDescriptors = 1;
    descriptor_ranges[1].BaseShaderRegister = 2;
    descriptor_ranges[1].RegisterSpace = 0;
    descriptor_ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[3].DescriptorTable.NumDescriptorRanges = 2;
    root_parameters[3].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 4;
    root_signature_desc.pParameters = root_parameters;
    hr = create_root_signature(device, &root_signature_desc, &context.root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    compute_pso = create_compute_pipeline_state(device, context.root_signature,
            shader_bytecode(cs_code, sizeof(cs_code)));

    context.pipeline_state = create_pipeline_state(device,
            context.root_signature, context.render_target_desc.Format,
            NULL, &ps, NULL);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 20;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "Failed to create descriptor heap, hr %#x.\n", hr);

    cs_cb = create_upload_buffer(device, sizeof(cs_data), &cs_data);
    ps_cb = create_upload_buffer(device, sizeof(ps_data), &ps_data);

    cs_raw_buffer = create_upload_buffer(device, sizeof(cs_raw_data), &cs_raw_data);
    ps_raw_buffer = create_upload_buffer(device, sizeof(ps_raw_data), &ps_raw_data);

    cs_raw_uav_buffer = create_default_buffer(device, sizeof(uint32_t),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ps_raw_uav_buffer = create_default_buffer(device, sizeof(ps_raw_uav_data),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_buffer_data(ps_raw_uav_buffer, 0, sizeof(ps_raw_uav_data), &ps_raw_uav_data, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, ps_raw_uav_buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cs_textures[0] = create_texture(device,
            1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    data.pData = &cs_data;
    data.RowPitch = sizeof(cs_data);
    data.SlicePitch = data.RowPitch;
    upload_texture_data(cs_textures[0], &data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, cs_textures[0],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cs_textures[1] = create_default_texture(device, 1, 1, DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    data.pData = &cs_texture_data;
    data.RowPitch = sizeof(cs_texture_data);
    data.SlicePitch = data.RowPitch;
    upload_texture_data(cs_textures[1], &data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, cs_textures[1],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ps_textures[0] = create_texture(device,
            1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    data.pData = &ps_data;
    data.RowPitch = sizeof(ps_data);
    data.SlicePitch = data.RowPitch;
    upload_texture_data(ps_textures[0], &data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, ps_textures[0],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ps_textures[1] = create_default_texture(device, 1, 1, DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    data.pData = &ps_texture_data;
    data.RowPitch = sizeof(ps_texture_data);
    data.SlicePitch = data.RowPitch;
    upload_texture_data(ps_textures[1], &data, 1, queue, command_list);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, ps_textures[1],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12Device_CreateShaderResourceView(device, cs_textures[0], NULL,
            get_cpu_descriptor_handle(&context, heap, 0));
    ID3D12Device_CreateUnorderedAccessView(device, cs_textures[1], NULL, NULL,
            get_cpu_descriptor_handle(&context, heap, 1));

    ID3D12Device_CreateShaderResourceView(device, ps_textures[0], NULL,
            get_cpu_descriptor_handle(&context, heap, 10));
    ID3D12Device_CreateUnorderedAccessView(device, ps_textures[1], NULL, NULL,
            get_cpu_descriptor_handle(&context, heap, 11));

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);

    ID3D12GraphicsCommandList_SetComputeRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &heap);

    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(command_list,
            0, ID3D12Resource_GetGPUVirtualAddress(cs_cb));
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(command_list,
            1, ID3D12Resource_GetGPUVirtualAddress(cs_raw_buffer));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(command_list,
            2, ID3D12Resource_GetGPUVirtualAddress(cs_raw_uav_buffer));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(command_list,
            3, get_gpu_descriptor_handle(&context, heap, 0));

    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list,
            0, ID3D12Resource_GetGPUVirtualAddress(ps_cb));
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(command_list,
            1, ID3D12Resource_GetGPUVirtualAddress(ps_raw_buffer));
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(command_list,
            2, ID3D12Resource_GetGPUVirtualAddress(ps_raw_uav_buffer));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list,
            3, get_gpu_descriptor_handle(&context, heap, 10));

    ID3D12GraphicsCommandList_SetPipelineState(command_list, compute_pso);
    ID3D12GraphicsCommandList_Dispatch(command_list, 1, 1, 1);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target, 0, queue, command_list, 0xff00ff00, 0);
    reset_command_list(command_list, context.allocator);
    transition_resource_state(command_list, cs_raw_uav_buffer,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(cs_raw_uav_buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    check_readback_data_uint(&rb, NULL, 0xffffffff, 0);
    release_resource_readback(&rb);

    ID3D12Resource_Release(cs_cb);
    ID3D12Resource_Release(ps_cb);
    ID3D12Resource_Release(cs_raw_buffer);
    ID3D12Resource_Release(cs_raw_uav_buffer);
    ID3D12Resource_Release(ps_raw_buffer);
    ID3D12Resource_Release(ps_raw_uav_buffer);
    for (i = 0; i < ARRAY_SIZE(cs_textures); ++i)
        ID3D12Resource_Release(cs_textures[i]);
    for (i = 0; i < ARRAY_SIZE(ps_textures); ++i)
        ID3D12Resource_Release(ps_textures[i]);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12PipelineState_Release(compute_pso);
    destroy_test_context(&context);
}

START_TEST(d3d12)
{
    bool enable_debug_layer = false;
    ID3D12Debug *debug;
    unsigned int i;

    for (i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--validate"))
            enable_debug_layer = true;
        else if (!strcmp(argv[i], "--warp"))
            use_warp_device = true;
    }

    if (enable_debug_layer && SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug)))
    {
        ID3D12Debug_EnableDebugLayer(debug);
        ID3D12Debug_Release(debug);
    }

    run_test(test_create_device);
    run_test(test_node_count);
    run_test(test_check_feature_support);
    run_test(test_format_support);
    run_test(test_create_command_allocator);
    run_test(test_create_command_list);
    run_test(test_create_command_queue);
    run_test(test_create_committed_resource);
    run_test(test_create_descriptor_heap);
    run_test(test_create_sampler);
    run_test(test_create_unordered_access_view);
    run_test(test_create_root_signature);
    run_test(test_root_signature_limits);
    run_test(test_create_pipeline_state);
    run_test(test_create_fence);
    run_test(test_reset_command_allocator);
    run_test(test_cpu_signal_fence);
    run_test(test_gpu_signal_fence);
    run_test(test_multithread_fence_wait);
    run_test(test_clear_depth_stencil_view);
    run_test(test_clear_render_target_view);
    run_test(test_clear_unordered_access_view);
    run_test(test_draw_instanced);
    run_test(test_draw_indexed_instanced);
    run_test(test_append_aligned_element);
    run_test(test_gpu_virtual_address);
    run_test(test_fragment_coords);
    run_test(test_fractional_viewports);
    run_test(test_scissor);
    run_test(test_draw_depth_only);
    run_test(test_draw_uav_only);
    run_test(test_texture_resource_barriers);
    run_test(test_invalid_texture_resource_barriers);
    run_test(test_device_removed_reason);
    run_test(test_map_resource);
    run_test(test_bundle_state_inheritance);
    run_test(test_shader_instructions);
    run_test(test_shader_interstage_interface);
    run_test(test_root_signature_byte_code);
    run_test(test_cs_constant_buffer);
    run_test(test_constant_buffer_relative_addressing);
    run_test(test_immediate_constant_buffer);
    run_test(test_root_constants);
    run_test(test_texture);
    run_test(test_gather);
    run_test(test_descriptor_tables);
    run_test(test_descriptor_tables_overlapping_bindings);
    run_test(test_update_root_descriptors);
    run_test(test_update_descriptor_tables);
    run_test(test_update_descriptor_heap_after_closing_command_list);
    run_test(test_update_compute_descriptor_tables);
    run_test(test_copy_descriptors);
    run_test(test_descriptors_visibility);
    run_test(test_get_copyable_footprints);
    run_test(test_depth_stencil_sampling);
    run_test(test_depth_load);
    run_test(test_typed_buffer_uav);
    run_test(test_typed_uav_store);
    run_test(test_compute_shader_registers);
    run_test(test_tgsm);
    run_test(test_uav_load);
    run_test(test_cs_uav_store);
    run_test(test_uav_counters);
    run_test(test_decrement_uav_counter);
    run_test(test_atomic_instructions);
    run_test(test_buffer_srv);
    run_test(test_create_query_heap);
    run_test(test_query_timestamp);
    run_test(test_query_pipeline_statistics);
    run_test(test_query_occlusion);
    run_test(test_resolve_non_issued_query_data);
    run_test(test_execute_indirect);
    run_test(test_dispatch_zero_thread_groups);
    run_test(test_instance_id);
    run_test(test_texture_copy_region);
    run_test(test_separate_bindings);
}
