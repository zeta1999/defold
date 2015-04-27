#include <assert.h>
#include <string.h>
#include <float.h>
#include <algorithm>

#include <dlib/hash.h>
#include <dlib/hashtable.h>
#include <dlib/profile.h>
#include <dlib/math.h>

#include <ddf/ddf.h>

#include "render_private.h"
#include "render_script.h"
#include "debug_renderer.h"
#include "font_renderer.h"

namespace dmRender
{
    using namespace Vectormath::Aos;

    const char* RENDER_SOCKET_NAME = "@render";

    StencilTestParams::StencilTestParams() {
        Init();
    }

    void StencilTestParams::Init() {
        m_Func = dmGraphics::COMPARE_FUNC_ALWAYS;
        m_OpSFail = dmGraphics::STENCIL_OP_KEEP;
        m_OpDPFail = dmGraphics::STENCIL_OP_KEEP;
        m_OpDPPass = dmGraphics::STENCIL_OP_KEEP;
        m_Ref = 0;
        m_RefMask = 0xff;
        m_BufferMask = 0xff;
        m_ColorBufferMask = 0xf;
        m_Padding = 0;
    }

    RenderObject::RenderObject()
    {
        Init();
    }

    void RenderObject::Init()
    {
        // See case 2264 why this method was added
        memset(this, 0, sizeof(RenderObject));
        m_WorldTransform = Matrix4::identity();
        m_TextureTransform = Matrix4::identity();

        for (uint32_t i = 0; i < RenderObject::MAX_CONSTANT_COUNT; ++i)
        {
            m_Constants[i].m_Location = -1;
        }
    }

    RenderContextParams::RenderContextParams()
    : m_ScriptContext(0x0)
    , m_SystemFontMap(0)
    , m_VertexProgramData(0x0)
    , m_FragmentProgramData(0x0)
    , m_MaxRenderTypes(0)
    , m_MaxInstances(0)
    , m_MaxRenderTargets(0)
    , m_VertexProgramDataSize(0)
    , m_FragmentProgramDataSize(0)
    , m_MaxCharacters(0)
    , m_CommandBufferSize(1024)
    , m_MaxDebugVertexCount(0)
    {

    }

    RenderScriptContext::RenderScriptContext()
    : m_LuaState(0)
    , m_CommandBufferSize(0)
    {

    }

    HRenderContext NewRenderContext(dmGraphics::HContext graphics_context, const RenderContextParams& params)
    {
        RenderContext* context = new RenderContext;

        context->m_RenderTargets.SetCapacity(params.m_MaxRenderTargets);

        context->m_RenderObjects.SetCapacity(params.m_MaxInstances);
        context->m_RenderObjects.SetSize(0);

        context->m_GraphicsContext = graphics_context;

        context->m_SystemFontMap = params.m_SystemFontMap;

        context->m_Material = 0;

        context->m_View = Matrix4::identity();
        context->m_Projection = Matrix4::identity();
        context->m_ViewProj = context->m_Projection * context->m_View;

        context->m_ScriptContext = params.m_ScriptContext;
        InitializeRenderScriptContext(context->m_RenderScriptContext, params.m_ScriptContext, params.m_CommandBufferSize);

        InitializeDebugRenderer(context, params.m_MaxDebugVertexCount, params.m_VertexProgramData, params.m_VertexProgramDataSize, params.m_FragmentProgramData, params.m_FragmentProgramDataSize);

        memset(context->m_Textures, 0, sizeof(dmGraphics::HTexture) * RenderObject::MAX_TEXTURE_COUNT);

        InitializeTextContext(context, params.m_MaxCharacters);

        context->m_OutOfResources = 0;

        context->m_RenderListDispatch.SetCapacity(256);

        dmMessage::Result r = dmMessage::NewSocket(RENDER_SOCKET_NAME, &context->m_Socket);
        assert(r == dmMessage::RESULT_OK);

        context->m_RenderListSortTarget = 0;

        return context;
    }

    Result DeleteRenderContext(HRenderContext render_context, dmScript::HContext script_context)
    {
        if (render_context == 0x0) return RESULT_INVALID_CONTEXT;

        FinalizeRenderScriptContext(render_context->m_RenderScriptContext, script_context);
        FinalizeDebugRenderer(render_context);
        FinalizeTextContext(render_context);
        dmMessage::DeleteSocket(render_context->m_Socket);
        delete render_context;

        return RESULT_OK;
    }

    dmScript::HContext GetScriptContext(HRenderContext render_context) {
        return render_context->m_ScriptContext;
    }

    void RenderListBegin(HRenderContext render_context)
    {
        render_context->m_RenderList.SetSize(0);
        render_context->m_RenderListSortIndices.SetSize(0);
        render_context->m_RenderListDispatch.SetSize(0);
    }

    HRenderListDispatch RenderListMakeDispatch(HRenderContext render_context, RenderListDispatchFn fn, void *user_data)
    {
        assert(render_context->m_RenderListDispatch.Size() < render_context->m_RenderListDispatch.Capacity());

        // store & return index
        RenderListDispatch d;
        d.m_Fn = fn;
        d.m_UserData = user_data;
        render_context->m_RenderListDispatch.Push(d);

        return render_context->m_RenderListDispatch.Size() - 1;
    }

    // Allocate a buffer (from the array) with room for 'entries' entries.
    //
    // NOTE: Pointer might go invalid after a consecutive call to RenderListAlloc if reallocatino
    //       of backing buffer happens.
    RenderListEntry* RenderListAlloc(HRenderContext render_context, uint32_t entries)
    {
        dmArray<RenderListEntry> & render_list = render_context->m_RenderList;

        if (render_list.Remaining() < entries)
        {
            const uint32_t needed = entries - render_list.Remaining();
            render_list.OffsetCapacity(dmMath::Max<uint32_t>(256, needed));
            render_context->m_RenderListSortIndices.SetCapacity(render_list.Capacity());
        }

        uint32_t size = render_list.Size();
        render_list.SetSize(size + entries);
        return (render_list.Begin() + size);
    }

    // Submit a range of entries (pointers must be from a range allocated by RenderListAlloc, and not between two alloc calls).
    void RenderListSubmit(HRenderContext render_context, RenderListEntry *begin, RenderListEntry *end)
    {
        // Insert the used up indices into the sort buffer.
        assert(end - begin <= render_context->m_RenderListSortIndices.Remaining());

        // Transform pointers back to indices.
        RenderListEntry *base = render_context->m_RenderList.Begin();
        uint32_t *insert = render_context->m_RenderListSortIndices.End();

        for (RenderListEntry* i=begin;i!=end;i++)
            *insert++ = i - base;

        render_context->m_RenderListSortIndices.SetSize(render_context->m_RenderListSortIndices.Size() + (end - begin));
    }

    struct RenderListSorter
    {
        bool operator()(uint32_t a, uint32_t b) const
        {
            const RenderListSortValue& u = values[a];
            const RenderListSortValue& v = values[b];
            if (u.m_SortKey == v.m_SortKey)
                return a < b;
            return u.m_SortKey < v.m_SortKey;
        }
        RenderListSortValue* values;
    };

    void RenderListEnd(HRenderContext render_context)
    {
        // These will be sorted into when dispatched.
        render_context->m_RenderListSortBuffers[0].SetCapacity(render_context->m_RenderListSortIndices.Capacity());
        render_context->m_RenderListSortBuffers[0].SetSize(0);
        render_context->m_RenderListSortBuffers[1].SetCapacity(render_context->m_RenderListSortIndices.Capacity());
        render_context->m_RenderListSortBuffers[1].SetSize(0);
        render_context->m_RenderListSortTarget = 0;
    }

    void SetSystemFontMap(HRenderContext render_context, HFontMap font_map)
    {
        render_context->m_SystemFontMap = font_map;
    }

    Result RegisterRenderTarget(HRenderContext render_context, dmGraphics::HRenderTarget rendertarget, dmhash_t hash)
    {
        if (render_context == 0x0)
            return RESULT_INVALID_CONTEXT;
        if (render_context->m_RenderTargets.Full())
            return RESULT_BUFFER_IS_FULL;

        RenderTargetSetup setup;
        setup.m_RenderTarget = rendertarget;
        setup.m_Hash = hash;
        render_context->m_RenderTargets.Push(setup);

        return RESULT_OK;
    }

    dmGraphics::HRenderTarget GetRenderTarget(HRenderContext render_context, dmhash_t hash)
    {
        for (uint32_t i=0; i < render_context->m_RenderTargets.Size(); i++)
        {
            if (render_context->m_RenderTargets[i].m_Hash == hash)
                return render_context->m_RenderTargets[i].m_RenderTarget;
        }

        return 0x0;
    }

    dmGraphics::HContext GetGraphicsContext(HRenderContext render_context)
    {
        return render_context->m_GraphicsContext;
    }

    const Matrix4& GetViewProjectionMatrix(HRenderContext render_context)
    {
        return render_context->m_ViewProj;
    }

    void SetViewMatrix(HRenderContext render_context, const Matrix4& view)
    {
        render_context->m_View = view;
        render_context->m_ViewProj = render_context->m_Projection * view;
    }

    void SetProjectionMatrix(HRenderContext render_context, const Matrix4& projection)
    {
        render_context->m_Projection = projection;
        render_context->m_ViewProj = projection * render_context->m_View;
    }

    Result AddToRender(HRenderContext context, RenderObject* ro)
    {
        if (context == 0x0) return RESULT_INVALID_CONTEXT;
        if (context->m_RenderObjects.Full())
        {
            if (!context->m_OutOfResources)
            {
                dmLogWarning("Renderer is out of resources, some objects will not be rendered.");
                context->m_OutOfResources = 1;
            }
            return RESULT_OUT_OF_RESOURCES;
        }
        context->m_RenderObjects.Push(ro);

        return RESULT_OK;
    }

    Result ClearRenderObjects(HRenderContext context)
    {
        context->m_RenderObjects.SetSize(0);
        ClearDebugRenderObjects(context);

        // Should probably be moved and/or refactored, see case 2261
        context->m_TextContext.m_RenderObjectIndex = 0;
        context->m_TextContext.m_VertexIndex = 0;
        context->m_TextContext.m_VerticesFlushed = 0;
        context->m_TextContext.m_TextBuffer.SetSize(0);
        context->m_TextContext.m_Batches.Clear();
        context->m_TextContext.m_TextEntries.SetSize(0);

        return RESULT_OK;
    }

    static void ApplyStencilTest(HRenderContext render_context, const RenderObject* ro)
    {
        dmGraphics::HContext graphics_context = dmRender::GetGraphicsContext(render_context);
        const StencilTestParams& stp = ro->m_StencilTestParams;
        dmGraphics::SetColorMask(graphics_context, stp.m_ColorBufferMask & (1<<3), stp.m_ColorBufferMask & (1<<2), stp.m_ColorBufferMask & (1<<1), stp.m_ColorBufferMask & (1<<0));
        dmGraphics::SetStencilMask(graphics_context, stp.m_BufferMask);
        dmGraphics::SetStencilFunc(graphics_context, stp.m_Func, stp.m_Ref, stp.m_RefMask);
        dmGraphics::SetStencilOp(graphics_context, stp.m_OpSFail, stp.m_OpDPFail, stp.m_OpDPPass);
    }

    static void ApplyRenderObjectConstants(HRenderContext render_context, const RenderObject* ro)
    {
        dmGraphics::HContext graphics_context = dmRender::GetGraphicsContext(render_context);

        for (uint32_t i = 0; i < RenderObject::MAX_CONSTANT_COUNT; ++i)
        {
            const Constant* c = &ro->m_Constants[i];
            if (c->m_Location != -1)
            {
                dmGraphics::SetConstantV4(graphics_context, &c->m_Value, c->m_Location);
            }
        }
    }

    static void MakeSortValues(HRenderContext context)
    {
        const uint32_t count = context->m_RenderListSortIndices.Size();

        // This is where the values go
        context->m_RenderListSortValues.SetCapacity(context->m_RenderListSortIndices.Capacity());
        context->m_RenderListSortValues.SetSize(context->m_RenderListSortIndices.Size());
        RenderListSortValue* sort_values = context->m_RenderListSortValues.Begin();

        const Matrix4& transform = context->m_ViewProj;
        RenderListEntry *base = context->m_RenderList.Begin();

        float minZW = FLT_MAX;
        float maxZW = -FLT_MAX;

        // Write z values and compute range
        int c = 0;
        for (uint32_t i=0;i!=count;i++)
        {
            uint32_t idx = context->m_RenderListSortIndices[i];
            RenderListEntry *entry = &base[idx];
            if (entry->m_MajorOrder != RENDER_ORDER_WORLD)
                continue;

            const Point3& world_pos = entry->m_WorldPosition;
            const Vector4 tmp(world_pos.getX(), world_pos.getY(), world_pos.getZ(), 1.0f);
            const Vector4 res = transform * tmp;
            const float zw = res.getZ() / res.getW();
            sort_values[idx].m_ZW = zw;
            if (zw < minZW) minZW = zw;
            if (zw > maxZW) maxZW = zw;
            c++;
        }

        float rc = 0;
        if (c > 1 && maxZW != minZW)
            rc = 1.0f / (maxZW - minZW);

        for (uint32_t i=0;i!=count;i++)
        {
            uint32_t idx = context->m_RenderListSortIndices[i];
            RenderListEntry *entry = &base[idx];

            sort_values[idx].m_MajorOrder = entry->m_MajorOrder;
            if (entry->m_MajorOrder == RENDER_ORDER_WORLD)
            {
                const float z = sort_values[idx].m_ZW;
                sort_values[idx].m_Order = (uint32_t) (0xfffff8 - 0xfffff0 * rc * (z - minZW));
            }
            else
            {
                // use the integer value provided.
                sort_values[idx].m_Order = entry->m_Order;
            }

            sort_values[idx].m_BatchKey = entry->m_BatchKey & 0xffffff;
            sort_values[idx].m_Dispatch = entry->m_Dispatch;
        }
    }

    Result DrawRenderList(HRenderContext context, Predicate* predicate, HNamedConstantBuffer constant_buffer)
    {
        uint32_t target = context->m_RenderListSortTarget;
        dmArray<uint32_t>* sort_target   = &context->m_RenderListSortBuffers[target];
        dmArray<uint32_t>* sort_previous = &context->m_RenderListSortBuffers[1-target];

        if (sort_previous->Size() == 0)
        {
            // First time drawing, set up sort keys.
            MakeSortValues(context);
        }
        else if (!memcmp(&context->m_RenderListDispatchedForViewProj, &context->m_ViewProj, sizeof(Matrix4)))
        {
            // Re-use same render object set if projection/view is exactly the same.
            return Draw(context, predicate, constant_buffer);
        }

        MakeSortValues(context);

        // Now to the actual sorting.
        const uint32_t count = context->m_RenderListSortIndices.Size();
        uint32_t* sort_indices = sort_target->Begin();
        memcpy(sort_indices, context->m_RenderListSortIndices.Begin(), sizeof(uint32_t) * count);
        sort_target->SetSize(count);

        RenderListSorter sort;
        sort.values = context->m_RenderListSortValues.Begin();
        std::sort(sort_indices, sort_indices + count, sort);

        // If previous sorting existed which is the same, use that
        if (sort_previous->Size() == sort_target->Size() && !memcmp(sort_previous->Begin(), sort_target->Begin(), sizeof(uint32_t) * count))
        {
            return Draw(context, predicate, constant_buffer);
        }

        context->m_RenderListDispatchedForViewProj = context->m_ViewProj;
        context->m_RenderObjects.SetSize(0);

        // No previous; swap buffers and build
        context->m_RenderListSortTarget ^= 1;

        RenderListDispatchParams params;
        memset(&params, 0x00, sizeof(params));
        params.m_Operation = RENDER_LIST_OPERATION_BEGIN;
        params.m_Context = context;

        // All get begin operation first
        for (uint32_t i=0;i!=context->m_RenderListDispatch.Size();i++)
        {
            const RenderListDispatch& d = context->m_RenderListDispatch[i];
            params.m_UserData = d.m_UserData;
            d.m_Fn(params);
        }

        params.m_Operation = RENDER_LIST_OPERATION_BATCH;
        params.m_Buf = context->m_RenderList.Begin();

        // Make batches for matching dispatch & batch key
        RenderListEntry *base = context->m_RenderList.Begin();
        uint32_t *last = sort_indices;

        for (uint32_t i=1;i<=count;i++)
        {
            uint32_t *idx = &sort_indices[i];
            const RenderListEntry *last_entry = &base[*last];

            // continue batch on match, or dispatch
            if (i < count && (last_entry->m_Dispatch == base[*idx].m_Dispatch && last_entry->m_BatchKey == base[*idx].m_BatchKey))
                continue;

            assert(last_entry->m_Dispatch < context->m_RenderListDispatch.Size());
            const RenderListDispatch* d = &context->m_RenderListDispatch[last_entry->m_Dispatch];

            params.m_UserData = d->m_UserData;
            params.m_Begin = last;
            params.m_End = idx;
            d->m_Fn(params);
            last = idx;
        }

        params.m_Operation = RENDER_LIST_OPERATION_END;
        params.m_Begin = 0;
        params.m_End = 0;
        params.m_Buf = 0;

        for (uint32_t i=0;i!=context->m_RenderListDispatch.Size();i++)
        {
            const RenderListDispatch& d = context->m_RenderListDispatch[i];
            params.m_UserData = d.m_UserData;
            d.m_Fn(params);
        }

        return Draw(context, predicate, constant_buffer);
    }

    Result Draw(HRenderContext render_context, Predicate* predicate, HNamedConstantBuffer constant_buffer)
    {
        if (render_context == 0x0)
            return RESULT_INVALID_CONTEXT;

        uint32_t tag_mask = 0;
        if (predicate != 0x0)
            tag_mask = ConvertMaterialTagsToMask(&predicate->m_Tags[0], predicate->m_TagCount);

        dmGraphics::HContext context = dmRender::GetGraphicsContext(render_context);

        // TODO: Move to "BeginFrame()" or similar? See case 2261
        FlushDebug(render_context);

        // Write vertex buffer
        FlushTexts(render_context, true);

        for (uint32_t i = 0; i < render_context->m_RenderObjects.Size(); ++i)
        {
            RenderObject* ro = render_context->m_RenderObjects[i];

            if (ro->m_VertexCount > 0 && (GetMaterialTagMask(ro->m_Material) & tag_mask) == tag_mask)
            {
                HMaterial material = ro->m_Material;
                if (render_context->m_Material)
                    material = render_context->m_Material;

                dmGraphics::EnableProgram(context, GetMaterialProgram(material));
                ApplyMaterialConstants(render_context, material, ro);
                ApplyMaterialSamplers(render_context, material);
                ApplyRenderObjectConstants(render_context, ro);

                if (constant_buffer)
                    ApplyNamedConstantBuffer(render_context, material, constant_buffer);

                if (ro->m_SetBlendFactors)
                    dmGraphics::SetBlendFunc(context, ro->m_SourceBlendFactor, ro->m_DestinationBlendFactor);

                if (ro->m_SetStencilTest)
                    ApplyStencilTest(render_context, ro);

                for (uint32_t i = 0; i < RenderObject::MAX_TEXTURE_COUNT; ++i)
                {
                    dmGraphics::HTexture texture = ro->m_Textures[i];
                    if (render_context->m_Textures[i])
                        texture = render_context->m_Textures[i];
                    if (texture)
                        dmGraphics::EnableTexture(context, i, texture);
                }

                dmGraphics::EnableVertexDeclaration(context, ro->m_VertexDeclaration, ro->m_VertexBuffer, GetMaterialProgram(material));

                if (ro->m_IndexBuffer)
                    dmGraphics::DrawElements(context, ro->m_PrimitiveType, ro->m_VertexCount, ro->m_IndexType, ro->m_IndexBuffer);
                else
                    dmGraphics::Draw(context, ro->m_PrimitiveType, ro->m_VertexStart, ro->m_VertexCount);

                dmGraphics::DisableVertexDeclaration(context, ro->m_VertexDeclaration);

                for (uint32_t i = 0; i < RenderObject::MAX_TEXTURE_COUNT; ++i)
                {
                    dmGraphics::HTexture texture = ro->m_Textures[i];
                    if (render_context->m_Textures[i])
                        texture = render_context->m_Textures[i];
                    if (texture)
                        dmGraphics::DisableTexture(context, i, texture);
                }

            }
        }
        return RESULT_OK;
    }

    Result DrawDebug3d(HRenderContext context)
    {
        return Draw(context, &context->m_DebugRenderer.m_3dPredicate, 0);
    }

    Result DrawDebug2d(HRenderContext context)
    {
        return Draw(context, &context->m_DebugRenderer.m_2dPredicate, 0);
    }

    void EnableRenderObjectConstant(RenderObject* ro, dmhash_t name_hash, const Vector4& value)
    {
        assert(ro);
        HMaterial material = ro->m_Material;
        assert(material);

        int32_t location = GetMaterialConstantLocation(material, name_hash);
        if (location == -1)
        {
            // Unknown constant, ie at least not defined in material
            return;
        }

        for (uint32_t i = 0; i < RenderObject::MAX_CONSTANT_COUNT; ++i)
        {
            Constant* c = &ro->m_Constants[i];
            if (c->m_Location == -1 || c->m_NameHash == name_hash)
            {
                // New or current slot found
                if (location != -1)
                {
                    c->m_Value = value;
                    c->m_NameHash = name_hash;
                    c->m_Type = dmRenderDDF::MaterialDesc::CONSTANT_TYPE_USER;
                    c->m_Location = location;
                    return;
                }
            }
        }

        dmLogError("Out of per object constant slots, max %d, when setting constant %s ", RenderObject::MAX_CONSTANT_COUNT, (const char*) dmHashReverse64(name_hash, 0));
    }

    void DisableRenderObjectConstant(RenderObject* ro, dmhash_t name_hash)
    {
        assert(ro);
        for (uint32_t i = 0; i < RenderObject::MAX_CONSTANT_COUNT; ++i)
        {
            Constant* c = &ro->m_Constants[i];
            if (c->m_NameHash == name_hash)
            {
                c->m_Location = -1;
                return;
            }
        }
    }


    struct NamedConstantBuffer
    {
        dmHashTable64<Vectormath::Aos::Vector4> m_Constants;
    };

    HNamedConstantBuffer NewNamedConstantBuffer()
    {
        HNamedConstantBuffer buffer = new NamedConstantBuffer();
        buffer->m_Constants.SetCapacity(16, 8);
        return buffer;
    }

    void DeleteNamedConstantBuffer(HNamedConstantBuffer buffer)
    {
        delete buffer;
    }

    void SetNamedConstant(HNamedConstantBuffer buffer, const char* name, Vectormath::Aos::Vector4 value)
    {
        dmHashTable64<Vectormath::Aos::Vector4>& constants = buffer->m_Constants;
        if (constants.Full())
        {
            uint32_t capacity = constants.Capacity();
            capacity += 8;
            constants.SetCapacity(capacity * 2, capacity);
        }
        constants.Put(dmHashString64(name), value);
    }

    bool GetNamedConstant(HNamedConstantBuffer buffer, const char* name, Vectormath::Aos::Vector4& value)
    {
        Vectormath::Aos::Vector4*v = buffer->m_Constants.Get(dmHashString64(name));
        if (v)
        {
            value = *v;
            return true;
        }
        else
        {
            return false;
        }
    }

    struct ApplyContext
    {
        dmGraphics::HContext m_GraphicsContext;
        HMaterial            m_Material;
        ApplyContext(dmGraphics::HContext graphics_context, HMaterial material)
        {
            m_GraphicsContext = graphics_context;
            m_Material = material;
        }
    };

    static inline void ApplyConstant(ApplyContext* context, const uint64_t* name_hash, Vectormath::Aos::Vector4* value)
    {
        int32_t* location = context->m_Material->m_NameHashToLocation.Get(*name_hash);
        if (location)
        {
            dmGraphics::SetConstantV4(context->m_GraphicsContext, value, *location);
        }
    }

    void ApplyNamedConstantBuffer(dmRender::HRenderContext render_context, HMaterial material, HNamedConstantBuffer buffer)
    {
        dmHashTable64<Vectormath::Aos::Vector4>& constants = buffer->m_Constants;
        dmGraphics::HContext graphics_context = dmRender::GetGraphicsContext(render_context);
        ApplyContext context(graphics_context, material);
        constants.Iterate(ApplyConstant, &context);
    }

}
