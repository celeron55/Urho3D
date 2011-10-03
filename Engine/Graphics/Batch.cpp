//
// Urho3D Engine
// Copyright (c) 2008-2011 Lasse ��rni
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "Camera.h"
#include "Geometry.h"
#include "Graphics.h"
#include "GraphicsImpl.h"
#include "Light.h"
#include "Material.h"
#include "Renderer.h"
#include "Profiler.h"
#include "ShaderVariation.h"
#include "Sort.h"
#include "Technique.h"
#include "Texture2D.h"
#include "VertexBuffer.h"

#include "DebugNew.h"

inline bool CompareBatchesFrontToBack(Batch* lhs, Batch* rhs)
{
    if (lhs->sortKey_ == rhs->sortKey_)
        return lhs->distance_ < rhs->distance_;
    else
        return lhs->sortKey_ > rhs->sortKey_;
}

inline bool CompareBatchesBackToFront(Batch* lhs, Batch* rhs)
{
    if (lhs->distance_ == rhs->distance_)
        return lhs->sortKey_ > rhs->sortKey_;
    else
        return lhs->distance_ > rhs->distance_;
}

inline bool CompareInstancesFrontToBack(const InstanceData& lhs, const InstanceData& rhs)
{
    return lhs.distance_ < rhs.distance_;
}

inline bool CompareBatchGroupsFrontToBack(BatchGroup* lhs, BatchGroup* rhs)
{
    return lhs->instances_[0].distance_ < rhs->instances_[0].distance_;
}

void Batch::CalculateSortKey()
{
    unsigned lightQueue = (*((unsigned*)&lightQueue_) / sizeof(LightBatchQueue)) & 0x7fff;
    unsigned pass = (*((unsigned*)&pass_) / sizeof(Pass)) & 0xffff;
    unsigned material = (*((unsigned*)&material_) / sizeof(Material)) & 0xffff;
    unsigned geometry = (*((unsigned*)&geometry_) / sizeof(Geometry)) & 0xffff;
    if (hasPriority_)
        lightQueue |= 0x8000;
    sortKey_ = (((unsigned long long)lightQueue) << 48) | (((unsigned long long)pass) << 32) |
        (((unsigned long long)material) << 16) | geometry;
}

void Batch::Prepare(Graphics* graphics, Renderer* renderer, const HashMap<StringHash, Vector4>& shaderParameters, bool setModelTransform) const
{
    if (!vertexShader_ || !pixelShader_)
        return;
    
    // Set pass / material-specific renderstates
    if (pass_ && material_)
    {
        if (pass_->GetAlphaTest())
            graphics->SetAlphaTest(true, CMP_GREATEREQUAL, 0.5f);
        else
            graphics->SetAlphaTest(false);
        
        graphics->SetBlendMode(pass_->GetBlendMode());
        graphics->SetCullMode(pass_->GetType() != PASS_SHADOW ? material_->GetCullMode() : material_->GetShadowCullMode());
        graphics->SetDepthTest(pass_->GetDepthTestMode());
        graphics->SetDepthWrite(pass_->GetDepthWrite());
    }
    
    // Set shaders
    graphics->SetShaders(vertexShader_, pixelShader_);
    
    // Set global shader parameters
    for (HashMap<StringHash, Vector4>::ConstIterator i = shaderParameters.Begin(); i != shaderParameters.End(); ++i)
    {
        if (graphics->NeedParameterUpdate(i->first_, &shaderParameters))
            graphics->SetShaderParameter(i->first_, i->second_);
    }
    
    // Set viewport and camera shader parameters
    if (graphics->NeedParameterUpdate(VSP_CAMERAPOS, camera_))
        graphics->SetShaderParameter(VSP_CAMERAPOS, camera_->GetWorldPosition());
    
    if (graphics->NeedParameterUpdate(VSP_CAMERAROT, camera_))
        graphics->SetShaderParameter(VSP_CAMERAROT, camera_->GetWorldTransform().RotationMatrix());
    
    if (overrideView_)
    {
        if (graphics->NeedParameterUpdate(VSP_VIEWPROJ, ((unsigned char*)camera_) + 4))
            graphics->SetShaderParameter(VSP_VIEWPROJ, camera_->GetProjection());
    }
    else
    {
        if (graphics->NeedParameterUpdate(VSP_VIEWPROJ, camera_))
            graphics->SetShaderParameter(VSP_VIEWPROJ, camera_->GetProjection() *
                camera_->GetInverseWorldTransform());
    }
    
    if (graphics->NeedParameterUpdate(VSP_VIEWRIGHTVECTOR, camera_))
        graphics->SetShaderParameter(VSP_VIEWRIGHTVECTOR, camera_->GetRightVector());
    
    if (graphics->NeedParameterUpdate(VSP_VIEWUPVECTOR, camera_))
        graphics->SetShaderParameter(VSP_VIEWUPVECTOR, camera_->GetUpVector());
    
    // Set model transform
    if (setModelTransform && graphics->NeedParameterUpdate(VSP_MODEL, worldTransform_))
        graphics->SetShaderParameter(VSP_MODEL, *worldTransform_);
    
    // Set skinning transforms
    if (shaderData_ && shaderDataSize_)
    {
        if (graphics->NeedParameterUpdate(VSP_SKINMATRICES, shaderData_))
            graphics->SetShaderParameter(VSP_SKINMATRICES, shaderData_, shaderDataSize_);
    }
    
    // Set light-related shader parameters
    Light* light = 0;
    Texture2D* shadowMap = 0;
    if (lightQueue_)
    {
        light = lightQueue_->light_;
        shadowMap = lightQueue_->shadowMap_;
        
        if (graphics->NeedParameterUpdate(VSP_LIGHTATTEN, light))
        {
            Vector4 lightAtten(1.0f / Max(light->GetRange(), M_EPSILON), 0.0f, 0.0f, 0.0f);
            graphics->SetShaderParameter(VSP_LIGHTATTEN, lightAtten);
        }
        
        if (graphics->NeedParameterUpdate(VSP_LIGHTDIR, light))
            graphics->SetShaderParameter(VSP_LIGHTDIR, light->GetWorldRotation() * Vector3::BACK);
        
        if (graphics->NeedParameterUpdate(VSP_LIGHTPOS, light))
            graphics->SetShaderParameter(VSP_LIGHTPOS, light->GetWorldPosition() - camera_->GetWorldPosition());
        
        if (graphics->NeedParameterUpdate(VSP_LIGHTVECROT, light))
        {
            Matrix3x4 lightVecRot(Vector3::ZERO, light->GetWorldRotation(), Vector3::UNITY);
            graphics->SetShaderParameter(VSP_LIGHTVECROT, lightVecRot);
        }
        
        if (graphics->NeedParameterUpdate(VSP_SPOTPROJ, light))
        {
            Matrix3x4 spotView(light->GetWorldPosition(), light->GetWorldRotation(), 1.0f);
            Matrix4 spotProj(Matrix4::ZERO);
            Matrix4 texAdjust(Matrix4::IDENTITY);
            
            // Make the projected light slightly smaller than the shadow map to prevent light spill
            float h = 1.005f / tanf(light->GetFov() * M_DEGTORAD * 0.5f);
            float w = h / light->GetAspectRatio();
            spotProj.m00_ = w;
            spotProj.m11_ = h;
            spotProj.m22_ = 1.0f / Max(light->GetRange(), M_EPSILON);
            spotProj.m32_ = 1.0f;
            
            #ifdef USE_OPENGL
            texAdjust.SetTranslation(Vector3(0.5f, 0.5f, 0.5f));
            texAdjust.SetScale(Vector3(0.5f, -0.5f, 0.5f));
            #else
            texAdjust.SetTranslation(Vector3(0.5f, 0.5f, 0.0f));
            texAdjust.SetScale(Vector3(0.5f, -0.5f, 1.0f));
            #endif
            
            graphics->SetShaderParameter(VSP_SPOTPROJ, texAdjust * spotProj * spotView.Inverse());
        }
        
        if (graphics->NeedParameterUpdate(PSP_LIGHTCOLOR, light))
        {
            float fade = 1.0f;
            float fadeEnd = light->GetDrawDistance();
            float fadeStart = light->GetFadeDistance();
            
            // Do fade calculation for light if both fade & draw distance defined
            if (light->GetLightType() != LIGHT_DIRECTIONAL && fadeEnd > 0.0f && fadeStart > 0.0f && fadeStart < fadeEnd)
                fade = Min(1.0f - (light->GetDistance() - fadeStart) / (fadeEnd - fadeStart), 1.0f);
            
            graphics->SetShaderParameter(PSP_LIGHTCOLOR, Vector4(light->GetColor().RGBValues(),
                light->GetSpecularIntensity()) * fade);
        }
        
        // Set shadow mapping shader parameters
        if (shadowMap)
        {
            if (graphics->NeedParameterUpdate(VSP_SHADOWPROJ, light))
            {
                Matrix4 shadowMatrices[MAX_CASCADE_SPLITS];
                
                unsigned numSplits = 1;
                if (light->GetLightType() == LIGHT_DIRECTIONAL)
                    numSplits = lightQueue_->shadowSplits_.Size();
                
                for (unsigned i = 0; i < numSplits; ++i)
                {
                    Camera* shadowCamera = lightQueue_->shadowSplits_[i].shadowCamera_;
                    const IntRect& viewport = lightQueue_->shadowSplits_[i].shadowViewport_;
                    
                    Matrix3x4 shadowView(shadowCamera->GetInverseWorldTransform());
                    Matrix4 shadowProj(shadowCamera->GetProjection());
                    Matrix4 texAdjust(Matrix4::IDENTITY);
                    
                    float width = (float)shadowMap->GetWidth();
                    float height = (float)shadowMap->GetHeight();
                    
                    Vector2 offset(
                        (float)viewport.left_ / width,
                        (float)viewport.top_ / height
                    );
                    
                    Vector2 scale(
                        0.5f * (float)(viewport.right_ - viewport.left_) / width,
                        0.5f * (float)(viewport.bottom_ - viewport.top_) / height
                    );
                    
                    #ifdef USE_OPENGL
                    offset.x_ += scale.x_;
                    offset.y_ += scale.y_;
                    offset.y_ = 1.0f - offset.y_;
                    // If using 4 shadow samples, offset the position diagonally by half pixel
                    if (renderer->GetShadowQuality() & SHADOWQUALITY_HIGH_16BIT)
                    {
                        offset.x_ -= 0.5f / width;
                        offset.y_ -= 0.5f / height;
                    }
                    texAdjust.SetTranslation(Vector3(offset.x_, offset.y_, 0.5f));
                    texAdjust.SetScale(Vector3(scale.x_, scale.y_, 0.5f));
                    #else
                    offset.x_ += scale.x_ + 0.5f / width;
                    offset.y_ += scale.y_ + 0.5f / height;
                    if (renderer->GetShadowQuality() & SHADOWQUALITY_HIGH_16BIT)
                    {
                        offset.x_ -= 0.5f / width;
                        offset.y_ -= 0.5f / height;
                    }
                    scale.y_ = -scale.y_;
                    texAdjust.SetTranslation(Vector3(offset.x_, offset.y_, 0.0f));
                    texAdjust.SetScale(Vector3(scale.x_, scale.y_, 1.0f));
                    #endif
                    
                    shadowMatrices[i] = texAdjust * shadowProj * shadowView;
                }
                
                graphics->SetShaderParameter(VSP_SHADOWPROJ, shadowMatrices[0].GetData(), 16 * numSplits);
            }
            
            if (graphics->NeedParameterUpdate(PSP_SAMPLEOFFSETS, shadowMap))
            {
                float addX = 1.0f / (float)shadowMap->GetWidth();
                float addY = 1.0f / (float)shadowMap->GetHeight();
                graphics->SetShaderParameter(PSP_SAMPLEOFFSETS, Vector4(addX, addY, 0.0f, 0.0f));
            }
            
            if (graphics->NeedParameterUpdate(PSP_SHADOWCUBEADJUST, light))
            {
                unsigned faceWidth = shadowMap->GetWidth() / 2;
                unsigned faceHeight = shadowMap->GetHeight() / 3;
                float width = (float)shadowMap->GetWidth();
                float height = (float)shadowMap->GetHeight();
                #ifdef USE_OPENGL
                    float mulX = (float)(faceWidth - 3) / width;
                    float mulY = (float)(faceHeight - 3) / height;
                    float addX = 1.5f / width;
                    float addY = 1.5f / height;
                #else
                    float mulX = (float)(faceWidth - 4) / width;
                    float mulY = (float)(faceHeight - 4) / height;
                    float addX = 2.5f / width;
                    float addY = 2.5f / height;
                #endif
                // If using 4 shadow samples, offset the position diagonally by half pixel
                if (renderer->GetShadowQuality() & SHADOWQUALITY_HIGH_16BIT)
                {
                    addX -= 0.5f / width;
                    addY -= 0.5f / height;
                }
                graphics->SetShaderParameter(PSP_SHADOWCUBEADJUST, Vector4(mulX, mulY, addX, addY));
            }
            
            if (graphics->NeedParameterUpdate(PSP_SHADOWCUBEPROJ, light))
            {
                // Note: we use the shadow camera of the first cube face. All are assumed to use the same projection
                Camera* shadowCamera = lightQueue_->shadowSplits_[0].shadowCamera_;
                float nearClip = shadowCamera->GetNearClip();
                float farClip = shadowCamera->GetFarClip();
                float q = farClip / (farClip - nearClip);
                float r = -q * nearClip;
                
                graphics->SetShaderParameter(PSP_SHADOWCUBEPROJ, Vector4(q, r, 0.0f, 0.0f));
            }
            
            if (graphics->NeedParameterUpdate(PSP_SHADOWFADE, light))
            {
                const CascadeParameters& parameters = light->GetShadowCascade();
                float farClip = camera_->GetFarClip();
                float shadowRange = parameters.GetShadowRange();
                float fadeStart = parameters.fadeStart_ * shadowRange / farClip;
                float fadeEnd = shadowRange / farClip;
                float fadeRange = fadeEnd - fadeStart;
                graphics->SetShaderParameter(PSP_SHADOWFADE, Vector4(fadeStart, 1.0f / fadeRange, 0.0f, 0.0f));
            }
            
            if (graphics->NeedParameterUpdate(PSP_SHADOWINTENSITY, light))
            {
                float intensity = light->GetShadowIntensity();
                float fadeStart = light->GetShadowFadeDistance();
                float fadeEnd = light->GetShadowDistance();
                if (fadeStart > 0.0f && fadeEnd > 0.0f && fadeEnd > fadeStart)
                    intensity = Lerp(intensity, 1.0f, Clamp((light->GetDistance() - fadeStart) / (fadeEnd - fadeStart), 0.0f, 1.0f));
                float pcfValues = (1.0f - intensity);
                // Fallback mode requires manual depth biasing. We do not do proper slope scale biasing,
                // instead just fudge the bias values together
                float constantBias = graphics->GetDepthConstantBias();
                float slopeScaledBias = graphics->GetDepthSlopeScaledBias();
                graphics->SetShaderParameter(PSP_SHADOWINTENSITY, Vector4(pcfValues, pcfValues * 0.25f, intensity, constantBias +
                    slopeScaledBias * constantBias));
            }
            
            if (graphics->NeedParameterUpdate(PSP_SHADOWSPLITS, light))
            {
                Vector4 lightSplits(M_LARGE_VALUE, M_LARGE_VALUE, M_LARGE_VALUE, M_LARGE_VALUE);
                if (lightQueue_->shadowSplits_.Size() > 1)
                    lightSplits.x_ = lightQueue_->shadowSplits_[0].farSplit_ / camera_->GetFarClip();
                if (lightQueue_->shadowSplits_.Size() > 2)
                    lightSplits.y_ = lightQueue_->shadowSplits_[1].farSplit_ / camera_->GetFarClip();
                if (lightQueue_->shadowSplits_.Size() > 3)
                    lightSplits.z_ = lightQueue_->shadowSplits_[2].farSplit_ / camera_->GetFarClip();
                
                graphics->SetShaderParameter(PSP_SHADOWSPLITS, lightSplits);
            }
        }
    }
    
    // Set material-specific shader parameters and textures
    if (material_)
    {
        const HashMap<StringHash, MaterialShaderParameter>& parameters = material_->GetShaderParameters();
        for (HashMap<StringHash, MaterialShaderParameter>::ConstIterator i = parameters.Begin(); i != parameters.End(); ++i)
        {
            if (graphics->NeedParameterUpdate(i->first_, material_))
                graphics->SetShaderParameter(i->first_, i->second_.value_);
        }
        
        const Vector<SharedPtr<Texture> >& textures = material_->GetTextures();
        if (graphics->NeedTextureUnit(TU_DIFFUSE))
            graphics->SetTexture(TU_DIFFUSE, textures[TU_DIFFUSE]);
        if (graphics->NeedTextureUnit(TU_NORMAL))
            graphics->SetTexture(TU_NORMAL, textures[TU_NORMAL]);
        if (graphics->NeedTextureUnit(TU_DETAIL))
            graphics->SetTexture(TU_DETAIL, textures[TU_DETAIL]);
        if (graphics->NeedTextureUnit(TU_ENVIRONMENT))
            graphics->SetTexture(TU_ENVIRONMENT, textures[TU_ENVIRONMENT]);
    }
    
    // Set light-related textures
    if (light)
    {
        if (shadowMap && graphics->NeedTextureUnit(TU_SHADOWMAP))
            graphics->SetTexture(TU_SHADOWMAP, shadowMap);
        if (graphics->NeedTextureUnit(TU_LIGHTRAMP))
        {
            Texture* rampTexture = light->GetRampTexture();
            if (!rampTexture)
                rampTexture = renderer->GetDefaultLightRamp();
            graphics->SetTexture(TU_LIGHTRAMP, rampTexture);
        }
        if (graphics->NeedTextureUnit(TU_LIGHTSHAPE))
        {
            Texture* shapeTexture = light->GetShapeTexture();
            if (!shapeTexture && light->GetLightType() == LIGHT_SPOT)
                shapeTexture = renderer->GetDefaultLightSpot();
            graphics->SetTexture(TU_LIGHTSHAPE, shapeTexture);
        }
    }
}

void Batch::Draw(Graphics* graphics, Renderer* renderer, const HashMap<StringHash, Vector4>& shaderParameters) const
{
    Prepare(graphics, renderer, shaderParameters);
    geometry_->Draw(graphics);
}

void BatchGroup::SetTransforms(Renderer* renderer, void* lockedData, unsigned& freeIndex)
{
    // Do not use up buffer space if not going to draw as instanced
    unsigned minGroupSize = renderer->GetMinInstanceGroupSize();
    unsigned maxIndexCount = renderer->GetMaxInstanceTriangles() * 3;
    if (instances_.Size() < minGroupSize || geometry_->GetIndexCount() > maxIndexCount)
        return;
    
    startIndex_ = freeIndex;
    Matrix3x4* dest = (Matrix3x4*)lockedData;
    dest += freeIndex;
    
    for (unsigned i = 0; i < instances_.Size(); ++i)
        *dest++ = *instances_[i].worldTransform_;
    
    freeIndex += instances_.Size();
}

void BatchGroup::Draw(Graphics* graphics, Renderer* renderer, const HashMap<StringHash, Vector4>& shaderParameters) const
{
    if (!instances_.Size())
        return;
    
    // Construct a temporary batch for rendering
    Batch batch;
    batch.geometry_ = geometry_;
    batch.material_ = material_;
    batch.pass_ = pass_;
    batch.vertexShader_ = vertexShader_;
    batch.pixelShader_ = pixelShader_;
    batch.camera_ = camera_;
    batch.lightQueue_ = lightQueue_;
    batch.vertexShaderIndex_ = vertexShaderIndex_;
    
    unsigned minGroupSize = renderer->GetMinInstanceGroupSize();
    unsigned maxIndexCount = renderer->GetMaxInstanceTriangles() * 3;
    
    // Draw as individual instances if below minimum size, or if instancing not supported
    VertexBuffer* instanceBuffer = renderer->GetInstancingBuffer();
    if (!instanceBuffer || instances_.Size() < minGroupSize || geometry_->GetIndexCount() > maxIndexCount)
    {
        batch.Prepare(graphics, renderer, shaderParameters, false);
        
        graphics->SetIndexBuffer(geometry_->GetIndexBuffer());
        graphics->SetVertexBuffers(geometry_->GetVertexBuffers(), geometry_->GetVertexElementMasks());
        
        for (unsigned i = 0; i < instances_.Size(); ++i)
        {
            graphics->SetShaderParameter(VSP_MODEL, *instances_[i].worldTransform_);
            graphics->Draw(geometry_->GetPrimitiveType(), geometry_->GetIndexStart(), geometry_->GetIndexCount(),
                geometry_->GetVertexStart(), geometry_->GetVertexCount());
        }
        
        graphics->ClearTransformSources();
    }
    else
    {
        // Switch to the instancing vertex shader
        // The indexing is different in the forward lit passes
        Vector<SharedPtr<ShaderVariation> >& vertexShaders = pass_->GetVertexShaders();
        Vector<SharedPtr<ShaderVariation> >& pixelShaders = pass_->GetPixelShaders();
        PassType type = pass_->GetType();
        if (type == PASS_LIGHT || type == PASS_LITBASE)
            batch.vertexShader_ = vertexShaders[vertexShaderIndex_ + GEOM_INSTANCED * MAX_LIGHT_VS_VARIATIONS];
        else
            batch.vertexShader_ = vertexShaders[vertexShaderIndex_ + GEOM_INSTANCED];
        
        batch.Prepare(graphics, renderer, shaderParameters, false);
        
        // Get the geometry vertex buffers, then add the instancing stream buffer
        // Hack: use a const_cast to avoid dynamic allocation of new temp vectors
        Vector<SharedPtr<VertexBuffer> >& vertexBuffers = const_cast<Vector<SharedPtr<VertexBuffer> >&>
            (geometry_->GetVertexBuffers());
        PODVector<unsigned>& elementMasks = const_cast<PODVector<unsigned>&>(geometry_->GetVertexElementMasks());
        vertexBuffers.Push(SharedPtr<VertexBuffer>(instanceBuffer));
        elementMasks.Push(instanceBuffer->GetElementMask());
        
        // No stream offset support, instancing buffer not pre-filled with transforms: have to lock and fill now
        if (startIndex_ == M_MAX_UNSIGNED)
        {
            unsigned startIndex = 0;
            while (startIndex < instances_.Size())
            {
                unsigned instances = instances_.Size() - startIndex;
                if (instances > instanceBuffer->GetVertexCount())
                    instances = instanceBuffer->GetVertexCount();
                
                // Lock the instance stream buffer and copy the transforms
                void* data = instanceBuffer->Lock(0, instances, LOCK_DISCARD);
                if (!data)
                {
                    // Remember to remove the instancing buffer and element mask
                    vertexBuffers.Pop();
                    elementMasks.Pop();
                    return;
                }
                Matrix3x4* dest = (Matrix3x4*)data;
                for (unsigned i = 0; i < instances; ++i)
                    dest[i] = *instances_[i + startIndex].worldTransform_;
                instanceBuffer->Unlock();
                
                graphics->SetIndexBuffer(geometry_->GetIndexBuffer());
                graphics->SetVertexBuffers(vertexBuffers, elementMasks);
                graphics->DrawInstanced(geometry_->GetPrimitiveType(), geometry_->GetIndexStart(), geometry_->GetIndexCount(),
                    geometry_->GetVertexStart(), geometry_->GetVertexCount(), instances);
                
                startIndex += instances;
            }
        }
        // Stream offset supported, and instancing buffer has been already filled, so just draw
        else
        {
            graphics->SetIndexBuffer(geometry_->GetIndexBuffer());
            graphics->SetVertexBuffers(vertexBuffers, elementMasks, startIndex_);
            graphics->DrawInstanced(geometry_->GetPrimitiveType(), geometry_->GetIndexStart(), geometry_->GetIndexCount(),
                geometry_->GetVertexStart(), geometry_->GetVertexCount(), instances_.Size());
        }
        
        // Remove the instancing buffer & element mask now
        vertexBuffers.Pop();
        elementMasks.Pop();
    }
}

void BatchQueue::Clear()
{
    batches_.Clear();
    sortedPriorityBatches_.Clear();
    sortedBatches_.Clear();
    priorityBatchGroups_.Clear();
    batchGroups_.Clear();
}

void BatchQueue::AddBatch(const Batch& batch, bool noInstancing)
{
    // If batch is something else than static, has custom view, or has per-instance shader data defined, can not instance
    if (noInstancing || batch.geometryType_ != GEOM_STATIC || batch.overrideView_ || batch.shaderData_)
        batches_.Push(batch);
    else
    {
        BatchGroupKey key;
        key.lightQueue_ = batch.lightQueue_;
        key.pass_ = batch.pass_;
        key.material_ = batch.material_;
        key.geometry_ = batch.geometry_;
        
        Map<BatchGroupKey, BatchGroup>* groups = batch.hasPriority_ ? &priorityBatchGroups_ : &batchGroups_;
        
        Map<BatchGroupKey, BatchGroup>::Iterator i = groups->Find(key);
        if (i == groups->End())
        {
            // Create new group
            BatchGroup newGroup;
            newGroup.geometry_ = batch.geometry_;
            newGroup.material_ = batch.material_;
            newGroup.pass_ = batch.pass_;
            newGroup.vertexShader_ = batch.vertexShader_;
            newGroup.pixelShader_ = batch.pixelShader_;
            newGroup.camera_ = batch.camera_;
            newGroup.lightQueue_ = batch.lightQueue_;
            newGroup.vertexShaderIndex_ = batch.vertexShaderIndex_;
            newGroup.instances_.Push(InstanceData(batch.worldTransform_, batch.distance_));
            groups->Insert(MakePair(key, newGroup));
        }
        else
            i->second_.instances_.Push(InstanceData(batch.worldTransform_, batch.distance_));
    }
}

void BatchQueue::SortBackToFront()
{
    sortedPriorityBatches_.Clear();
    sortedBatches_.Resize(batches_.Size());
    
    for (unsigned i = 0; i < batches_.Size(); ++i)
        sortedBatches_[i] = &batches_[i];
    
    Sort(sortedBatches_.Begin(), sortedBatches_.End(), CompareBatchesBackToFront);
    
    // Do not actually sort batch groups, just list them
    sortedPriorityBatchGroups_.Resize(priorityBatchGroups_.Size());
    sortedBatchGroups_.Resize(batchGroups_.Size());
    
    unsigned index = 0;
    for (Map<BatchGroupKey, BatchGroup>::Iterator i = priorityBatchGroups_.Begin(); i != priorityBatchGroups_.End(); ++i)
        sortedPriorityBatchGroups_[index++] = &i->second_;
    index = 0;
    for (Map<BatchGroupKey, BatchGroup>::Iterator i = batchGroups_.Begin(); i != batchGroups_.End(); ++i)
        sortedBatchGroups_[index++] = &i->second_;
}

void BatchQueue::SortFrontToBack()
{
    sortedPriorityBatches_.Clear();
    sortedBatches_.Clear();
    
    // Must explicitly divide into priority batches and non-priority, so that priorities do not get mixed up between
    // instanced and non-instanced batches
    for (unsigned i = 0; i < batches_.Size(); ++i)
    {
        if (batches_[i].hasPriority_)
            sortedPriorityBatches_.Push(&batches_[i]);
        else
            sortedBatches_.Push(&batches_[i]);
    }
    
    Sort(sortedPriorityBatches_.Begin(), sortedPriorityBatches_.End(), CompareBatchesFrontToBack);
    Sort(sortedBatches_.Begin(), sortedBatches_.End(), CompareBatchesFrontToBack);
    
    // Sort each group front to back
    for (Map<BatchGroupKey, BatchGroup>::Iterator i = priorityBatchGroups_.Begin(); i != priorityBatchGroups_.End(); ++i)
        Sort(i->second_.instances_.Begin(), i->second_.instances_.End(), CompareInstancesFrontToBack);
    for (Map<BatchGroupKey, BatchGroup>::Iterator i = batchGroups_.Begin(); i != batchGroups_.End(); ++i)
        Sort(i->second_.instances_.Begin(), i->second_.instances_.End(), CompareInstancesFrontToBack);
    
    // Now sort batch groups by the distance of the first batch
    sortedPriorityBatchGroups_.Resize(priorityBatchGroups_.Size());
    sortedBatchGroups_.Resize(batchGroups_.Size());
    
    unsigned index = 0;
    for (Map<BatchGroupKey, BatchGroup>::Iterator i = priorityBatchGroups_.Begin(); i != priorityBatchGroups_.End(); ++i)
        sortedPriorityBatchGroups_[index++] = &i->second_;
    index = 0;
    for (Map<BatchGroupKey, BatchGroup>::Iterator i = batchGroups_.Begin(); i != batchGroups_.End(); ++i)
        sortedBatchGroups_[index++] = &i->second_;
    
    Sort(sortedPriorityBatchGroups_.Begin(), sortedPriorityBatchGroups_.End(), CompareBatchGroupsFrontToBack);
    Sort(sortedBatchGroups_.Begin(), sortedBatchGroups_.End(), CompareBatchGroupsFrontToBack);
}

void BatchQueue::SetTransforms(Renderer* renderer, void* lockedData, unsigned& freeIndex)
{
    for (Map<BatchGroupKey, BatchGroup>::Iterator i = priorityBatchGroups_.Begin(); i != priorityBatchGroups_.End(); ++i)
        i->second_.SetTransforms(renderer, lockedData, freeIndex);
    for (Map<BatchGroupKey, BatchGroup>::Iterator i = batchGroups_.Begin(); i != batchGroups_.End(); ++i)
        i->second_.SetTransforms(renderer, lockedData, freeIndex);
}

unsigned BatchQueue::GetNumInstances(Renderer* renderer) const
{
    unsigned total = 0;
    unsigned minGroupSize = renderer->GetMinInstanceGroupSize();
    unsigned maxIndexCount = renderer->GetMaxInstanceTriangles() * 3;
    
    // This is for the purpose of calculating how much space is needed in the instancing buffer. Do not add instance counts
    // that are below the minimum threshold for instancing
    for (Map<BatchGroupKey, BatchGroup>::ConstIterator i = priorityBatchGroups_.Begin(); i != priorityBatchGroups_.End(); ++i)
    {
        unsigned instances = i->second_.instances_.Size();
        if (instances >= minGroupSize && i->second_.geometry_->GetIndexCount() <= maxIndexCount)
            total += instances;
    }
    for (Map<BatchGroupKey, BatchGroup>::ConstIterator i = batchGroups_.Begin(); i != batchGroups_.End(); ++i)
    {
        unsigned instances = i->second_.instances_.Size();
        if (instances >= minGroupSize && i->second_.geometry_->GetIndexCount() <= maxIndexCount)
            total += instances;
    }
    
    return total;
}
