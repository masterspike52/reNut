/**
 * @file        reporter/ui_imgui_sdl.cpp
 * @brief       The crash reporter window on Linux: ImGui over SDL's renderer
 *
 * @remarks     This program runs because something already went badly wrong,
 *              quite possibly in the graphics driver, so it must degrade rather
 *              than fail. SDL's renderer tries the GPU through OpenGL or Vulkan
 *              and falls back to its own software renderer, which needs no
 *              driver at all. The SDK builds its SDL without the renderer, so
 *              the reporter is its own CMake project with an SDL of its own.
 */

#include <string>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "reporter.h"
#include "ui_imgui.h"

namespace reporter {

bool RunImGuiUi() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    g_session.window_error = SDL_GetError();
    return false;
  }
  SDL_Window* window = SDL_CreateWindow(WindowTitle().c_str(), 900, 700, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    g_session.window_error = SDL_GetError();
    SDL_Quit();
    return false;
  }
  // Letting SDL choose walks its drivers in order and ends on the software one when nothing else starts.
  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer) {
    g_session.window_error = SDL_GetError();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return false;
  }
  SDL_SetRenderVSync(renderer, 1);
  SDL_ShowWindow(window);
  SDL_RaiseWindow(window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;  // Never leave settings files behind.
  ApplyStyle();
  const float scale = SDL_GetWindowDisplayScale(window);
  if (scale > 0.0f) {
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
  }
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  while (!g_quit) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))) g_quit = true;
    }
    if (g_quit) break;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    DrawWindow();
    ImGui::Render();

    SDL_SetRenderScale(renderer, ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(renderer, 0.09f, 0.09f, 0.11f, 1.0f);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  FinishWindow();
  return true;
}

}  // namespace reporter
