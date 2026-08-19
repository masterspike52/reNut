#include "renut_engine/game_activity_stats.h"

#include "rex_macros.h"
#include "globals.h"

void gameActivityAnimationStream(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::RecordAnimationStream();
}

void gameActivityAnimationStreamEnd(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::FinishAnimationStream();
}

void gameActivityBodyBlend(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::RecordBodyBlend();
}

void gameActivityBodyBlendEnd(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::FinishBodyBlend();
}

void gameActivityActorScript(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::RecordActorScript();
}

void gameActivityActorScriptEnd(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::FinishActorScript();
}

void gameActivityActorGeneration(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::RecordActorGeneration();
}

void gameActivityActorGenerationEnd(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::FinishActorGeneration();
}

void PresentNativeFrame() {}

void nativeSetVertexShader(PPCRegister&, PPCRegister&) {}
void nativeSetPixelShader(PPCRegister&, PPCRegister&) {}
void nativeSetVertexDeclaration(PPCRegister&, PPCRegister&) {}
void nativeCreateVertexDeclarationBegin(PPCRegister&) {}
void nativeCreateVertexDeclarationEnd(PPCRegister&) {}
void nativeSetStreamSource(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetIndices(PPCRegister&, PPCRegister&) {}
void nativeSetTexture(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetVertexConstants(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetPixelConstants(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetRenderTarget(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetDepthStencil(PPCRegister&, PPCRegister&) {}
void nativeSetViewport(PPCRegister&, PPCRegister&) {}
void nativeSetScissor(PPCRegister&, PPCRegister&) {}
void nativeDrawIndexed(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeDraw(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeClear(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeResolve(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSwap(PPCRegister&, PPCRegister&, PPCRegister&) {
    renut::game_activity_stats::EndFrame();
}

void nativeSetAlphaBlendEnable(PPCRegister&, PPCRegister&) {}
void nativeSetSrcBlend(PPCRegister&, PPCRegister&) {}
void nativeSetDestBlend(PPCRegister&, PPCRegister&) {}
void nativeSetBlendOp(PPCRegister&, PPCRegister&) {}
void nativeSetSeparateAlphaBlendEnable(PPCRegister&, PPCRegister&) {}
void nativeSetSrcBlendAlpha(PPCRegister&, PPCRegister&) {}
void nativeSetDestBlendAlpha(PPCRegister&, PPCRegister&) {}
void nativeSetBlendOpAlpha(PPCRegister&, PPCRegister&) {}
void nativeSetBlendFactor(PPCRegister&, PPCRegister&) {}
void nativeSetAlphaTestEnable(PPCRegister&, PPCRegister&) {}
void nativeSetAlphaRef(PPCRegister&, PPCRegister&) {}
void nativeSetAlphaFunc(PPCRegister&, PPCRegister&) {}
void nativeSetZEnable(PPCRegister&, PPCRegister&) {}
void nativeSetZWriteEnable(PPCRegister&, PPCRegister&) {}
void nativeSetZFunc(PPCRegister&, PPCRegister&) {}
void nativeSetCullMode(PPCRegister&, PPCRegister&) {}
void nativeSetFillMode(PPCRegister&, PPCRegister&) {}
void nativeSetColorWriteEnable(PPCRegister&, PPCRegister&) {}
void nativeSetScissorEnable(PPCRegister&, PPCRegister&) {}
void nativeSetDepthBias(PPCRegister&, PPCRegister&) {}
void nativeSetSlopeScaleDepthBias(PPCRegister&, PPCRegister&) {}
void nativeSetStencilEnable(PPCRegister&, PPCRegister&) {}
void nativeSetTwoSidedStencilMode(PPCRegister&, PPCRegister&) {}
void nativeSetStencilFunc(PPCRegister&, PPCRegister&) {}
void nativeSetStencilFail(PPCRegister&, PPCRegister&) {}
void nativeSetStencilZFail(PPCRegister&, PPCRegister&) {}
void nativeSetStencilPass(PPCRegister&, PPCRegister&) {}
void nativeSetStencilRef(PPCRegister&, PPCRegister&) {}
void nativeSetStencilMask(PPCRegister&, PPCRegister&) {}
void nativeSetStencilWriteMask(PPCRegister&, PPCRegister&) {}
void nativeSetCCWStencilFunc(PPCRegister&, PPCRegister&) {}
void nativeSetCCWStencilFail(PPCRegister&, PPCRegister&) {}
void nativeSetCCWStencilZFail(PPCRegister&, PPCRegister&) {}
void nativeSetCCWStencilPass(PPCRegister&, PPCRegister&) {}

void nativeSetSamplerMinFilter(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerMagFilter(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerMipFilter(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerAddressU(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerAddressV(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerAddressW(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerMaxAnisotropy(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerMipMapLodBias(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerMaxMipLevel(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerMinMipLevel(PPCRegister&, PPCRegister&, PPCRegister&) {}
void nativeSetSamplerBorderColor(PPCRegister&, PPCRegister&, PPCRegister&) {}
