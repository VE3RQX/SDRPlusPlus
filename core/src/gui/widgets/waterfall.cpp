#include <gui/widgets/waterfall.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imutils.h>
#include <algorithm>
#include <volk/volk.h>
#include <utils/flog.h>
#include <gui/gui.h>
#include <gui/style.h>

float DEFAULT_COLOR_MAP[][3] = {
    { 0x00, 0x00, 0x20 },
    { 0x00, 0x00, 0x30 },
    { 0x00, 0x00, 0x50 },
    { 0x00, 0x00, 0x91 },
    { 0x1E, 0x90, 0xFF },
    { 0xFF, 0xFF, 0xFF },
    { 0xFF, 0xFF, 0x00 },
    { 0xFE, 0x6D, 0x16 },
    { 0xFF, 0x00, 0x00 },
    { 0xC6, 0x00, 0x00 },
    { 0x9F, 0x00, 0x00 },
    { 0x75, 0x00, 0x00 },
    { 0x4A, 0x00, 0x00 }
};

inline double findBestRange(double bandwidth, int maxSteps) {

    double step = std::pow(10, std::floor(std::log10(bandwidth/maxSteps)));

    for(int i = 0; i < 3; ++i) {
        if(bandwidth/step <= maxSteps)
            break;
        step *= (i & 1) ? 2.5 : 2.0;
    }
    return step;
}

inline int format_frequency(char *buf, size_t nbuf, double freq) {
    uint64_t F = fabs(freq);

    if (F < 1000)
        return snprintf(buf, nbuf, "%" PRIu64, F);

    if (F < 1000000)
        return snprintf(buf, nbuf, "%" PRIu64 ".%03" PRIu64,
                    F/1000, F % 1000);

    if (F < 1000000000)
        return snprintf(buf, nbuf, "%" PRIu64 ".%03" PRIu64 ".%03" PRIu64,
                            F/1000000, (F / 1000) % 1000, F % 1000);

    return snprintf(buf, nbuf, "%" PRIu64 ".%03" PRIu64 ".%03" PRIu64 ".%03" PRIu64,
                            F/1000000000, (F / 1000000) % 1000, (F / 1000) % 1000, F % 1000);
}

inline int format_bandwidth(char *buf, size_t nbuf, double bandwidth) {

    bandwidth = std::floor(bandwidth);  // ,,, ?

    if (bandwidth < 1000)
        return snprintf(buf, nbuf, "%g", bandwidth);

    if (bandwidth < 1000000)
        return snprintf(buf, nbuf, "%gk", bandwidth/1000.0);

    if (bandwidth < 1000000000)
        return snprintf(buf, nbuf, "%gM", bandwidth/1000000.0);

    return snprintf(buf, nbuf, "%gG", bandwidth/1000000000.0);
}

namespace ImGui {

    WaterFall::WaterFall() {
        fftMin = -70.0;
        fftMax = 0.0;
        waterfallMin = -70.0;
        waterfallMax = 0.0;
        FFTAreaHeight = 300;
        newFFTAreaHeight = FFTAreaHeight;
        fftHeight = FFTAreaHeight - 50;
        dataWidth = 600;
        lastWidgetPos.x = 0;
        lastWidgetPos.y = 0;
        lastWidgetSize.x = 0;
        lastWidgetSize.y = 0;
        latestFFT = new float[dataWidth];
        latestFFTHold = new float[dataWidth];
        fullUpdateBuf = new float[dataWidth];
        waterfallFb = new uint32_t[1];

        viewBandwidth = 1.0;
        wholeBandwidth = 1.0;

        updatePallette(DEFAULT_COLOR_MAP, 13);
    }

    void WaterFall::init() {
        glGenTextures(1, &textureId);
    }

    void WaterFall::drawFFT() {
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        // Calculate scaling factor
        float startLine = floorf(fftMax / vRange) * vRange;
        float vertRange = fftMax - fftMin;
        float scaleFactor = fftHeight / vertRange;
        char buf[100];

        ImU32 trace = ImGui::GetColorU32(ImGuiCol_PlotLines);
        ImU32 traceHold = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftHoldColor);
        ImU32 shadow = ImGui::GetColorU32(ImGuiCol_PlotLines, 0.2);
        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        float textVOffset = 10.0f * style::uiScale;

        // Vertical scale
        for (float line = startLine; line > fftMin; line -= vRange) {
            float yPos = fftAreaMax.y - ((line - fftMin) * scaleFactor);
            window->DrawList->AddLine(ImVec2(fftAreaMin.x, roundf(yPos)),
                                      ImVec2(fftAreaMax.x, roundf(yPos)),
                                      IM_COL32(50, 50, 50, 255), style::uiScale);
            sprintf(buf, "%d", (int)line);
            ImVec2 txtSz = ImGui::CalcTextSize(buf);
            window->DrawList->AddText(ImVec2(fftAreaMin.x - txtSz.x - textVOffset, roundf(yPos - (txtSz.y / 2.0))), text, buf);
        }

        // Horizontal scale

        double horizScale = (double) dataWidth / viewBandwidth;

        auto horz_project = [&](double f) {
            return fftAreaMin.x + ((f - lowerFreq) * horizScale);
        };

        const float   tall_tick = 11 * style::uiScale;
        const float medium_tick =  7 * style::uiScale;
        const float  short_tick =  4 * style::uiScale;

        double freq_index = floorf(lowerFreq/range);

        for(;;) {
            double freq = freq_index*range;

            if(freq >= upperFreq)
                break;
            freq_index += 1.0;

            if(freq >= lowerFreq) {
                double xPos = horz_project(freq);

                //
                // vertical line in spectrum
                //
                window->DrawList->AddLine(ImVec2(roundf(xPos), fftAreaMin.y + 1),
                                          ImVec2(roundf(xPos), fftAreaMax.y),
                                              IM_COL32(50, 50, 50, 255), style::uiScale);

                //
                // tick for frequency label
                //
                window->DrawList->AddLine(ImVec2(roundf(xPos), fftAreaMax.y),
                                          ImVec2(roundf(xPos), fftAreaMax.y + tall_tick),
                                              text, style::uiScale);

                //
                // frequency label
                //
                format_frequency(buf, sizeof(buf), freq);
                ImVec2 txtSz = ImGui::CalcTextSize(buf);

                window->DrawList->AddText(ImVec2(roundf(xPos - (txtSz.x / 2.0)), fftAreaMax.y + txtSz.y), text, buf);
            }

            //
            // unlabeled ticks between labels
            //
            for(int u = 1; u < 10; ++u) {
                double uf = freq + u*range/10.0;

                if(uf < lowerFreq)
                    continue;

                if(uf > upperFreq)
                    break;

                double xPos = horz_project(freq + u*range/10.0);

                //
                // tick for frequency label
                //
                float tick = (u == 5) ? medium_tick : short_tick;

                window->DrawList->AddLine(ImVec2(roundf(xPos), fftAreaMax.y),
                                          ImVec2(roundf(xPos), fftAreaMax.y + tick),
                                              text, style::uiScale);
            }
        }

        // Data
        if (latestFFT != NULL && fftLines != 0) {
            for (int i = 1; i < dataWidth; i++) {
                double aPos = fftAreaMax.y - ((latestFFT[i - 1] - fftMin) * scaleFactor);
                double bPos = fftAreaMax.y - ((latestFFT[i] - fftMin) * scaleFactor);
                aPos = std::clamp<double>(aPos, fftAreaMin.y + 1, fftAreaMax.y);
                bPos = std::clamp<double>(bPos, fftAreaMin.y + 1, fftAreaMax.y);
                window->DrawList->AddLine(ImVec2(fftAreaMin.x + i - 1, roundf(aPos)),
                                          ImVec2(fftAreaMin.x + i, roundf(bPos)), trace, 1.0);
                window->DrawList->AddLine(ImVec2(fftAreaMin.x + i, roundf(bPos)),
                                          ImVec2(fftAreaMin.x + i, fftAreaMax.y), shadow, 1.0);
            }
        }

        // Hold
        if (fftHold && latestFFT != NULL && latestFFTHold != NULL && fftLines != 0) {
            for (int i = 1; i < dataWidth; i++) {
                double aPos = fftAreaMax.y - ((latestFFTHold[i - 1] - fftMin) * scaleFactor);
                double bPos = fftAreaMax.y - ((latestFFTHold[i] - fftMin) * scaleFactor);
                aPos = std::clamp<double>(aPos, fftAreaMin.y + 1, fftAreaMax.y);
                bPos = std::clamp<double>(bPos, fftAreaMin.y + 1, fftAreaMax.y);
                window->DrawList->AddLine(ImVec2(fftAreaMin.x + i - 1, roundf(aPos)),
                                          ImVec2(fftAreaMin.x + i, roundf(bPos)), traceHold, 1.0);
            }
        }

        FFTRedrawArgs args;
        args.min = fftAreaMin;
        args.max = fftAreaMax;
        args.lowFreq = lowerFreq;
        args.highFreq = upperFreq;
        args.freqToPixelRatio = horizScale;
        args.pixelToFreqRatio = viewBandwidth / (double)dataWidth;
        args.window = window;
        onFFTRedraw.emit(args);

        // X Axis
        window->DrawList->AddLine(ImVec2(fftAreaMin.x, fftAreaMax.y),
                                  ImVec2(fftAreaMax.x, fftAreaMax.y),
                                  text, style::uiScale);
        // Y Axis
        window->DrawList->AddLine(ImVec2(fftAreaMin.x, fftAreaMin.y),
                                  ImVec2(fftAreaMin.x, fftAreaMax.y - 1),
                                  text, style::uiScale);
    }

    void WaterFall::drawWaterfall() {
        if (waterfallUpdate) {
            waterfallUpdate = false;
            updateWaterfallTexture();
        }
        {
            std::lock_guard<std::mutex> lck(texMtx);
            window->DrawList->AddImage((void*)(intptr_t)textureId, wfMin, wfMax);
        }

        ImVec2 mPos = ImGui::GetMousePos();

        if (IS_IN_AREA(mPos, wfMin, wfMax) && !gui::mainWindow.lockWaterfallControls && !inputHandled) {
            for (auto const& [name, vfo] : vfos) {
                window->DrawList->AddRectFilled(vfo->wfRectMin, vfo->wfRectMax, vfo->color);
                if (!vfo->lineVisible) { continue; }
                window->DrawList->AddLine(vfo->wfLineMin, vfo->wfLineMax, (name == selectedVFO) ? IM_COL32(255, 0, 0, 255) : IM_COL32(255, 255, 0, 255), style::uiScale);
            }
        }
    }

    void WaterFall::drawVFOs() {
        for (auto const& [name, vfo] : vfos) {
            vfo->draw(window, name == selectedVFO);
        }
    }

    void WaterFall::selectFirstVFO() {
        bool available = false;
        for (auto const& [name, vfo] : vfos) {
            available = true;
            selectedVFO = name;
            selectedVFOChanged = true;
            return;
        }
        if (!available) {
            selectedVFO = "";
            selectedVFOChanged = true;
        }
    }

    void WaterFall::processInputs() {
        // Pre calculate useful values
        WaterfallVFO* selVfo = NULL;
        if (selectedVFO != "") {
            selVfo = vfos[selectedVFO];
        }
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        ImVec2 dragOrigin(mousePos.x - drag.x, mousePos.y - drag.y);

        bool mouseHovered, mouseHeld;
        bool mouseClicked = ImGui::ButtonBehavior(ImRect(fftAreaMin, wfMax), GetID("WaterfallID"), &mouseHovered, &mouseHeld,
                                                  ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_PressedOnClick);

        mouseInFFTResize = (dragOrigin.x > widgetPos.x && dragOrigin.x < widgetPos.x + widgetSize.x && dragOrigin.y >= widgetPos.y + newFFTAreaHeight - (2.0f * style::uiScale) && dragOrigin.y <= widgetPos.y + newFFTAreaHeight + (2.0f * style::uiScale));
        mouseInFreq = IS_IN_AREA(dragOrigin, freqAreaMin, freqAreaMax);
        mouseInFFT = IS_IN_AREA(dragOrigin, fftAreaMin, fftAreaMax);
        mouseInWaterfall = IS_IN_AREA(dragOrigin, wfMin, wfMax);

        int mouseWheel = ImGui::GetIO().MouseWheel;

        bool mouseMoved = false;
        if (mousePos.x != lastMousePos.x || mousePos.y != lastMousePos.y) { mouseMoved = true; }
        lastMousePos = mousePos;

        std::string hoveredVFOName = "";
        for (auto const& [name, _vfo] : vfos) {
            if (ImGui::IsMouseHoveringRect(_vfo->rectMin, _vfo->rectMax) || ImGui::IsMouseHoveringRect(_vfo->wfRectMin, _vfo->wfRectMax)) {
                hoveredVFOName = name;
                break;
            }
        }

        // Deselect everything if the mouse is released
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (fftResizeSelect) {
                FFTAreaHeight = newFFTAreaHeight;
                onResize();
            }

            fftResizeSelect = false;
            freqScaleSelect = false;
            vfoSelect = false;
            vfoBorderSelect = false;
            lastDrag = 0;
        }

        bool targetFound = false;

        // If the mouse was clicked anywhere in the waterfall, check if the resize was clicked
        if (mouseInFFTResize) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                fftResizeSelect = true;
                targetFound = true;
            }
        }

        // If mouse was clicked inside the central part, check what was clicked
        if (mouseClicked && !targetFound) {
            mouseDownPos = mousePos;

            // First, check if a VFO border was selected
            for (auto const& [name, _vfo] : vfos) {
                if (_vfo->bandwidthLocked) { continue; }
                if (_vfo->rectMax.x - _vfo->rectMin.x < 10) { continue; }
                bool resizing = false;
                if (_vfo->reference != REF_LOWER) {
                    if (IS_IN_AREA(mousePos, _vfo->lbwSelMin, _vfo->lbwSelMax)) { resizing = true; }
                    else if (IS_IN_AREA(mousePos, _vfo->wfLbwSelMin, _vfo->wfLbwSelMax)) {
                        resizing = true;
                    }
                }
                if (_vfo->reference != REF_UPPER) {
                    if (IS_IN_AREA(mousePos, _vfo->rbwSelMin, _vfo->rbwSelMax)) { resizing = true; }
                    else if (IS_IN_AREA(mousePos, _vfo->wfRbwSelMin, _vfo->wfRbwSelMax)) {
                        resizing = true;
                    }
                }
                if (!resizing) { continue; }
                relatedVfo = _vfo;
                vfoBorderSelect = true;
                targetFound = true;
                break;
            }

            // Next, check if a VFO was selected
            if (!targetFound && hoveredVFOName != "") {
                selectedVFO = hoveredVFOName;
                selectedVFOChanged = true;
                targetFound = true;
                return;
            }

            // Now, check frequency scale
            if (!targetFound && mouseInFreq) {
                freqScaleSelect = true;
            }
        }

        // If the FFT resize bar was selected, resize FFT accordingly
        if (fftResizeSelect) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            newFFTAreaHeight = mousePos.y - widgetPos.y;
            newFFTAreaHeight = std::clamp<float>(newFFTAreaHeight, 150, widgetSize.y - 50);
            ImGui::GetForegroundDrawList()->AddLine(ImVec2(widgetPos.x, newFFTAreaHeight + widgetPos.y), ImVec2(widgetEndPos.x, newFFTAreaHeight + widgetPos.y),
                                                    ImGui::GetColorU32(ImGuiCol_SeparatorActive), style::uiScale);
            return;
        }

        // If a vfo border is selected, resize VFO accordingly
        if (vfoBorderSelect) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            double dist = (relatedVfo->reference == REF_CENTER) ? fabsf(mousePos.x - relatedVfo->lineMin.x) : (mousePos.x - relatedVfo->lineMin.x);
            if (relatedVfo->reference == REF_UPPER) { dist = -dist; }
            double hzDist = dist * (viewBandwidth / (double)dataWidth);
            if (relatedVfo->reference == REF_CENTER) {
                hzDist *= 2.0;
            }
            hzDist = std::clamp<double>(hzDist, relatedVfo->minBandwidth, relatedVfo->maxBandwidth);
            relatedVfo->setBandwidth(hzDist);
            relatedVfo->onUserChangedBandwidth.emit(hzDist);
            return;
        }

        // If the frequency scale is selected, move it
        if (freqScaleSelect) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            double deltax = drag.x - lastDrag;
            lastDrag = drag.x;
            if(deltax == 0.0)
                return;
            double viewDelta = deltax * (viewBandwidth / (double)dataWidth);

            viewOffset -= viewDelta;

            if (viewOffset + (viewBandwidth / 2.0) > wholeBandwidth / 2.0) {
                double freqOffset = (viewOffset + (viewBandwidth / 2.0)) - (wholeBandwidth / 2.0);
                viewOffset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
                if (!centerFrequencyLocked) {
                    centerFreq += freqOffset;
                    centerFreqMoved = true;
                }
            }
            if (viewOffset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0)) {
                double freqOffset = (viewOffset - (viewBandwidth / 2.0)) + (wholeBandwidth / 2.0);
                viewOffset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
                if (!centerFrequencyLocked) {
                    centerFreq += freqOffset;
                    centerFreqMoved = true;
                }
            }

            lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
            upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);

            if (viewBandwidth != wholeBandwidth) {
                updateAllVFOs();
                if (_fullUpdate) { updateWaterfallFb(); };
            }
            return;
        }

        // If the mouse wheel is moved on the frequency scale
        if (mouseWheel != 0 && mouseInFreq) {
            viewOffset -= (double)mouseWheel * viewBandwidth / 20.0;

            if (viewOffset + (viewBandwidth / 2.0) > wholeBandwidth / 2.0) {
                double freqOffset = (viewOffset + (viewBandwidth / 2.0)) - (wholeBandwidth / 2.0);
                viewOffset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
                centerFreq += freqOffset;
                centerFreqMoved = true;
            }
            if (viewOffset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0)) {
                double freqOffset = (viewOffset - (viewBandwidth / 2.0)) + (wholeBandwidth / 2.0);
                viewOffset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
                centerFreq += freqOffset;
                centerFreqMoved = true;
            }

            lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
            upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);

            if (viewBandwidth != wholeBandwidth) {
                updateAllVFOs();
                if (_fullUpdate) { updateWaterfallFb(); };
            }
            return;
        }

        // If the left and right keys are pressed while hovering the freq scale, move it too
        bool leftKeyPressed = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        if ((leftKeyPressed || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) && mouseInFreq) {
            viewOffset += leftKeyPressed ? (viewBandwidth / 20.0) : (-viewBandwidth / 20.0);

            if (viewOffset + (viewBandwidth / 2.0) > wholeBandwidth / 2.0) {
                double freqOffset = (viewOffset + (viewBandwidth / 2.0)) - (wholeBandwidth / 2.0);
                viewOffset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
                centerFreq += freqOffset;
                centerFreqMoved = true;
            }
            if (viewOffset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0)) {
                double freqOffset = (viewOffset - (viewBandwidth / 2.0)) + (wholeBandwidth / 2.0);
                viewOffset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
                centerFreq += freqOffset;
                centerFreqMoved = true;
            }

            lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
            upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);

            if (viewBandwidth != wholeBandwidth) {
                updateAllVFOs();
                if (_fullUpdate) { updateWaterfallFb(); };
            }
            return;
        }

        // Finally, if nothing else was selected, just move the VFO
        if ((VFOMoveSingleClick ? ImGui::IsMouseClicked(ImGuiMouseButton_Left) : ImGui::IsMouseDown(ImGuiMouseButton_Left)) && (mouseInFFT | mouseInWaterfall) && (mouseMoved || hoveredVFOName == "")) {
            if (selVfo != NULL) {
                int refCenter = mousePos.x - fftAreaMin.x;
                if (refCenter >= 0 && refCenter < dataWidth) {
                    double off = ((((double)refCenter / ((double)dataWidth / 2.0)) - 1.0) * (viewBandwidth / 2.0)) + viewOffset;
                    off += centerFreq;
                    off = (round(off / selVfo->snapInterval) * selVfo->snapInterval) - centerFreq;
                    selVfo->setOffset(off);
                }
            }
        }
        else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Check if a VFO is hovered. If yes, show tooltip
            for (auto const& [name, _vfo] : vfos) {
                if (ImGui::IsMouseHoveringRect(_vfo->rectMin, _vfo->rectMax) || ImGui::IsMouseHoveringRect(_vfo->wfRectMin, _vfo->wfRectMax)) {
                    char buf[128];
                    ImGui::BeginTooltip();

                    ImGui::TextUnformatted(name.c_str());

                    if (ImGui::GetIO().KeyCtrl) {
                        ImGui::Separator();

                        format_frequency(buf, sizeof(buf), _vfo->generalOffset + centerFreq);
                        ImGui::Text("Frequency: %sHz", buf);

                        format_frequency(buf, sizeof(buf), _vfo->bandwidth);
                        ImGui::Text("Bandwidth: %sHz", buf);

                        ImGui::Text("Bandwidth Locked: %s", _vfo->bandwidthLocked ? "Yes" : "No");

                        float strength, snr;
                        if (calculateVFOSignalInfo(waterfallVisible ? &rawFFTs[currentFFTLine * rawFFTSize] : rawFFTs, _vfo, strength, snr)) {
                            ImGui::Text("Strength: %0.1fdBFS", strength);
                            ImGui::Text("SNR: %0.1fdB", snr);
                        }
                        else {
                            ImGui::TextUnformatted("Strength: ---.-dBFS");
                            ImGui::TextUnformatted("SNR: ---.-dB");
                        }
                    }

                    ImGui::EndTooltip();
                    break;
                }
            }
        }

        // Handle Page Up to cycle through VFOs
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp) && selVfo != NULL) {
            std::string next = (--vfos.end())->first;
            std::string lowest = "";
            double lowestOffset = INFINITY;
            double firstVfoOffset = selVfo->generalOffset;
            double smallestDistance = INFINITY;
            bool found = false;
            for (auto& [_name, _vfo] : vfos) {
                if (_vfo->generalOffset > firstVfoOffset && (_vfo->generalOffset - firstVfoOffset) < smallestDistance) {
                    next = _name;
                    smallestDistance = (_vfo->generalOffset - firstVfoOffset);
                    found = true;
                }
                if (_vfo->generalOffset < lowestOffset) {
                    lowestOffset = _vfo->generalOffset;
                    lowest = _name;
                }
            }
            selectedVFO = found ? next : lowest;
            selectedVFOChanged = true;
        }

        // Handle Page Down to cycle through VFOs
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && selVfo != NULL) {
            std::string next = (--vfos.end())->first;
            std::string highest = "";
            double highestOffset = -INFINITY;
            double firstVfoOffset = selVfo->generalOffset;
            double smallestDistance = INFINITY;
            bool found = false;
            for (auto& [_name, _vfo] : vfos) {
                if (_vfo->generalOffset < firstVfoOffset && (firstVfoOffset - _vfo->generalOffset) < smallestDistance) {
                    next = _name;
                    smallestDistance = (firstVfoOffset - _vfo->generalOffset);
                    found = true;
                }
                if (_vfo->generalOffset > highestOffset) {
                    highestOffset = _vfo->generalOffset;
                    highest = _name;
                }
            }
            selectedVFO = found ? next : highest;
            selectedVFOChanged = true;
        }
    }

    //
    // what this method is doing:
    //
    // a VFO is defined by a center frequency and a bandwidth.  we can draw this:
    //
    //     lower ... center ... upper
    //
    // where (upper - lower) == bandwidth of VFO, so (center - lower) == bandwidth/2.
    //
    // we extend the above by another bandwidth:
    //
    //     min ... lower ... center ... upper ... max
    //
    // where (max - min) == bandwidth, lower - min == bandwidth/2, etc.
    //
    // this method measures the combined power in the [min,lower] and [upper, max]
    // and calls this the "noise".   it measures the total power in [lower, upper]
    // and refers to that as the "signal".  the SNR is then the ratio.
    //
    // a few things:  the noise estimator depends on the areas outside the VFO to
    // actually be noise.  this assumption will be violated in crowded conditions.
    // it may be a useful thing to allow for the definition of a "noise VFO", and
    // let that be the noise estimate.
    //
    // the fft data being processed is given in dbFS, not linear power units.
    // sadly, the original method would work on dBFS data as if they were linear,
    // which had the unfortunate effect of enhancing lower powers and other distortions.
    //
    // the current version converts to linear power, and back to log for the SNR.
    //
    bool WaterFall::calculateVFOSignalInfo(float* fftLine, WaterfallVFO* _vfo, float& strength, float& snr) {
        if (fftLine == NULL || fftLines <= 0) { return false; }

        // Calculate FFT index data
        double vfoMinSizeFreq = _vfo->centerOffset - _vfo->bandwidth;
        double vfoMinFreq = _vfo->centerOffset - (_vfo->bandwidth / 2.0);
        double vfoMaxFreq = _vfo->centerOffset + (_vfo->bandwidth / 2.0);
        double vfoMaxSizeFreq = _vfo->centerOffset + _vfo->bandwidth;

        auto get_offset = [&](double freq) {
            return std::clamp<int>(((freq / (wholeBandwidth / 2.0)) * (double)(rawFFTSize / 2)) + (rawFFTSize / 2), 0, rawFFTSize);

        };

        int vfoMinSideOffset = get_offset(vfoMinSizeFreq),
            vfoMinOffset     = get_offset(    vfoMinFreq),
            vfoMaxOffset     = get_offset(    vfoMaxFreq),
            vfoMaxSideOffset = get_offset(vfoMaxSizeFreq);

        //
        // the noise
        //
        double noise = 0;
        int    n_noise = 0;

        for (int i = vfoMinSideOffset; i < vfoMinOffset; i++) {
            noise += std::exp(fftLine[i]*(M_LN10/10.0));
            ++n_noise;
        }

        for (int i = vfoMaxOffset + 1; i < vfoMaxSideOffset; i++) {
            noise += std::exp(fftLine[i]*(M_LN10/10.0));
            ++n_noise;
        }

        //
        // now the signal
        //
        double signal = 0;
        int    n_signal = 0;

        for (int i = vfoMinOffset; i <= vfoMaxOffset; i++) {
            signal += std::exp(fftLine[i]*(M_LN10/10.0));
            ++n_signal;
        }

        strength = 10.0*std::log10(signal);

        snr = 10.0*std::log10((signal*n_noise)/(noise*n_signal));

        return true;
    }

    void WaterFall::updateWaterfallFb() {
        if (!waterfallVisible || rawFFTs == NULL) {
            return;
        }

        Zoom zoom(*this);

        const float dataRange = waterfallMax - waterfallMin;
        const int count = std::min<float>(waterfallHeight, fftLines);

        if (rawFFTs != NULL && fftLines >= 0) {
            for (int i = 0; i < count; i++) {

                zoom(fullUpdateBuf, &rawFFTs[((i + currentFFTLine) % waterfallHeight) * rawFFTSize]);

                for (int j = 0; j < dataWidth; j++) {
                    float pixel = (std::clamp<float>(fullUpdateBuf[j], waterfallMin, waterfallMax) - waterfallMin) / dataRange;

                    waterfallFb[(i * dataWidth) + j] = waterfallPallet[(int)(pixel * (WATERFALL_RESOLUTION - 1))];
                }
            }

            for (int i = count; i < waterfallHeight; i++) {
                for (int j = 0; j < dataWidth; j++) {
                    waterfallFb[(i * dataWidth) + j] = (uint32_t)255 << 24;
                }
            }
        }
        waterfallUpdate = true;
    }

    void WaterFall::drawBandPlanRow(bandplan::BandPlan_t *plan, int row) {

        if(plan == NULL)
            return;
        plan->compile_bands();

        double horizScale = double(dataWidth) / viewBandwidth;

	float vertical_extension = 1.5f;

        float height = ImGui::CalcTextSize("0").y * vertical_extension;
        float bpBottom;

        if (bandPlanPos == BANDPLAN_POS_BOTTOM) {
            bpBottom = fftAreaMax.y - height*row;
        }
        else {
            bpBottom = fftAreaMin.y + height + 1 + height*row;
        }

        int count = plan->bands.size();

        for (int i = 0; i < count; i++) {
            auto &bar = plan->bands[i];

            double start = bar.start;
            double   end = bar.end;

            if (end < lowerFreq)
                continue;
            if (start >= upperFreq)
                break;

            struct {
                bool start;
                bool   end;
            } visible;
            visible.start = (start >= lowerFreq);
            visible.end   = (  end <  upperFreq);

            start = std::clamp<double>(start, lowerFreq, upperFreq);
            end   = std::clamp<double>(  end, lowerFreq, upperFreq);


            auto project = [&](double f) {
                return fftAreaMin.x + (f - lowerFreq)*horizScale;
            };

            double aPos = project(start);
            double bPos = project(end);
            double width = bPos - aPos;

            if (aPos <= fftAreaMin.x) {
                aPos = fftAreaMin.x + 1;
            }
            if (bPos <= fftAreaMin.x) {
                bPos = fftAreaMin.x + 1;
            }
            if (width >= 1.0) {

                int total_height = height;
                int total_labels = bar.labels.size();
                int total_position = bpBottom;

                for(const auto &l : bar.labels) {
                    auto &color = l.color;

                    int dh = total_height/total_labels;

                    if(l.type != "guard") {
                        window->DrawList->AddRectFilled(ImVec2(roundf(aPos), total_position - dh),
                                                        ImVec2(roundf(bPos), total_position),
                                                            color.trans);
                    } else {
                        window->DrawList->AddLine(ImVec2(roundf(aPos), total_position - dh),
                                                  ImVec2(roundf(bPos), total_position),
                                                            color.trans, style::uiScale);
                        window->DrawList->AddLine(ImVec2(roundf(aPos), total_position),
                                                  ImVec2(roundf(bPos), total_position - dh),
                                                            color.trans, style::uiScale);
                    }

                    if (visible.start && l.visible.start) {
                        window->DrawList->AddLine(ImVec2(roundf(aPos), total_position - dh),
                                                  ImVec2(roundf(aPos), total_position),
                                                      color.value, style::uiScale);
                    }
                    if (visible.end && l.visible.end) {
                        window->DrawList->AddLine(ImVec2(roundf(bPos), total_position - dh),
                                                  ImVec2(roundf(bPos), total_position),
                                                      color.value, style::uiScale);
                    }

                    total_position -= dh;
                    total_height -= dh;
                    total_labels -= 1;
                }
            }

            if (bar.labels.size() == 1) {
                ImVec2 txtSz;
                double cPos = project((start + end)/2.0);

                txtSz = ImGui::CalcTextSize(bar.labels[0].name.c_str());

                if(txtSz.x <= width) {
                    window->DrawList->AddText(ImVec2(cPos - (txtSz.x / 2.0), bpBottom - (height / 2.0f) - (txtSz.y / 2.0f)),
                                          IM_COL32(255, 255, 255, 255), bar.labels[0].name.c_str());
                }
            }
        }
    }

    void WaterFall::drawBandPlan() {
        drawBandPlanRow(bandplan[0], 0);
        drawBandPlanRow(bandplan[1], 1);
    }

    void WaterFall::updateWaterfallTexture() {
        std::lock_guard<std::mutex> lck(texMtx);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dataWidth, waterfallHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t*)waterfallFb);
    }

    void WaterFall::onPositionChange() {
        // Nothing to see here...
    }

    void WaterFall::onResize() {
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        std::lock_guard<std::mutex> lck2(smoothingBufMtx);
        // return if widget is too small
        if (widgetSize.x < 100 || widgetSize.y < 100) {
            return;
        }

        int lastWaterfallHeight = waterfallHeight;

        if (waterfallVisible) {
            FFTAreaHeight = std::min<int>(FFTAreaHeight, widgetSize.y - (50.0f * style::uiScale));
            newFFTAreaHeight = FFTAreaHeight;
            fftHeight = FFTAreaHeight - (50.0f * style::uiScale);
            waterfallHeight = widgetSize.y - fftHeight - (50.0f * style::uiScale) - 2;
        }
        else {
            fftHeight = widgetSize.y - (50.0f * style::uiScale);
        }
        dataWidth = widgetSize.x - (60.0f * style::uiScale);

        if (waterfallVisible) {
            // Raw FFT resize
            fftLines = std::min<int>(fftLines, waterfallHeight) - 1;
            if (rawFFTs != NULL) {
                if (currentFFTLine != 0) {
                    float* tempWF = new float[currentFFTLine * rawFFTSize];
                    int moveCount = lastWaterfallHeight - currentFFTLine;
                    memcpy(tempWF, rawFFTs, currentFFTLine * rawFFTSize * sizeof(float));
                    memmove(rawFFTs, &rawFFTs[currentFFTLine * rawFFTSize], moveCount * rawFFTSize * sizeof(float));
                    memcpy(&rawFFTs[moveCount * rawFFTSize], tempWF, currentFFTLine * rawFFTSize * sizeof(float));
                    delete[] tempWF;
                }
                currentFFTLine = 0;
                rawFFTs = (float*)realloc(rawFFTs, waterfallHeight * rawFFTSize * sizeof(float));
            }
            else {
                rawFFTs = (float*)malloc(waterfallHeight * rawFFTSize * sizeof(float));
            }
            // ==============
        }

        // Reallocate the full-waterfall update buf
        delete[] fullUpdateBuf;
        fullUpdateBuf = new float[dataWidth];

        // Reallocate display FFT
        delete[] latestFFT;
        latestFFT = new float[dataWidth];

        // Reallocate hold FFT
        delete[] latestFFTHold;
        latestFFTHold = new float[dataWidth];

        // Reallocate smoothing buffer
        delete[] smoothingBuf;
        if (fftSmoothing) {
            smoothingBuf = new float[dataWidth];
            for (int i = 0; i < dataWidth; i++) {
                smoothingBuf[i] = -1000.0f;
            }
        } else
            smoothingBuf = NULL;

        if (waterfallVisible) {
            delete[] waterfallFb;
            waterfallFb = new uint32_t[dataWidth * waterfallHeight];
            memset(waterfallFb, 0, dataWidth * waterfallHeight * sizeof(uint32_t));
        }
        for (int i = 0; i < dataWidth; i++) {
            latestFFT[i] = -1000.0f; // Hide everything
            latestFFTHold[i] = -1000.0f;
        }

        fftAreaMin = ImVec2(widgetPos.x + (50.0f * style::uiScale), widgetPos.y + (9.0f * style::uiScale));
        fftAreaMax = ImVec2(fftAreaMin.x + dataWidth, fftAreaMin.y + fftHeight + 1);

        freqAreaMin = ImVec2(fftAreaMin.x, fftAreaMax.y + 1);
        freqAreaMax = ImVec2(fftAreaMax.x, fftAreaMax.y + (40.0f * style::uiScale));

        wfMin = ImVec2(fftAreaMin.x, freqAreaMax.y + 1);
        wfMax = ImVec2(fftAreaMin.x + dataWidth, wfMin.y + waterfallHeight);

        maxHSteps = dataWidth / (3*ImGui::CalcTextSize("000.000").x); // + 10);
        maxVSteps = fftHeight / (  ImGui::CalcTextSize("000.000").y);

        range = findBestRange(viewBandwidth, maxHSteps);
        vRange = findBestRange(fftMax - fftMin, maxVSteps);

        updateWaterfallFb();
        updateAllVFOs();
    }

    void WaterFall::draw() {
        buf_mtx.lock();
        window = GetCurrentWindow();

        widgetPos = ImGui::GetWindowContentRegionMin();
        widgetEndPos = ImGui::GetWindowContentRegionMax();
        widgetPos.x += window->Pos.x;
        widgetPos.y += window->Pos.y;
        widgetEndPos.x += window->Pos.x - 4; // Padding
        widgetEndPos.y += window->Pos.y;
        widgetSize = ImVec2(widgetEndPos.x - widgetPos.x, widgetEndPos.y - widgetPos.y);

        if (selectedVFO == "" && vfos.size() > 0) {
            selectFirstVFO();
        }

        if (widgetPos.x != lastWidgetPos.x || widgetPos.y != lastWidgetPos.y) {
            lastWidgetPos = widgetPos;
            onPositionChange();
        }
        if (widgetSize.x != lastWidgetSize.x || widgetSize.y != lastWidgetSize.y) {
            lastWidgetSize = widgetSize;
            onResize();
        }

        //window->DrawList->AddRectFilled(widgetPos, widgetEndPos, IM_COL32( 0, 0, 0, 255 ));
        ImU32 bg = ImGui::ColorConvertFloat4ToU32(gui::themeManager.waterfallBg);
        window->DrawList->AddRectFilled(widgetPos, widgetEndPos, bg);
        window->DrawList->AddRect(widgetPos, widgetEndPos, IM_COL32(50, 50, 50, 255), 0.0, 0, style::uiScale);
        window->DrawList->AddLine(ImVec2(widgetPos.x, freqAreaMax.y), ImVec2(widgetPos.x + widgetSize.x, freqAreaMax.y), IM_COL32(50, 50, 50, 255), style::uiScale);

        if (!gui::mainWindow.lockWaterfallControls) {
            inputHandled = false;
            InputHandlerArgs args;
            args.fftRectMin = fftAreaMin;
            args.fftRectMax = fftAreaMax;
            args.freqScaleRectMin = freqAreaMin;
            args.freqScaleRectMax = freqAreaMax;
            args.waterfallRectMin = wfMin;
            args.waterfallRectMax = wfMax;
            args.lowFreq = lowerFreq;
            args.highFreq = upperFreq;
            args.freqToPixelRatio = (double)dataWidth / viewBandwidth;
            args.pixelToFreqRatio = viewBandwidth / (double)dataWidth;
            onInputProcess.emit(args);
            if (!inputHandled) { processInputs(); }
        }

        updateAllVFOs(true);

        drawFFT();
        if (waterfallVisible) {
            drawWaterfall();
        }
        drawVFOs();
        if (bandplan != NULL && bandplanVisible) {
            drawBandPlan();
        }

        if (!waterfallVisible) {
            buf_mtx.unlock();
            return;
        }

        buf_mtx.unlock();
    }

    float* WaterFall::getFFTBuffer() {
        if (rawFFTs == NULL) { return NULL; }
        buf_mtx.lock();
        if (waterfallVisible) {
            currentFFTLine--;
            fftLines++;
            currentFFTLine = ((currentFFTLine + waterfallHeight) % waterfallHeight);
            fftLines = std::min<float>(fftLines, waterfallHeight);
            return &rawFFTs[currentFFTLine * rawFFTSize];
        }
        return rawFFTs;
    }

    void WaterFall::pushFFT() {
        if (rawFFTs == NULL) { return; }
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);

        Zoom zoom(*this);

        if (waterfallVisible) {
            zoom(latestFFT, &rawFFTs[currentFFTLine * rawFFTSize]);
            memmove(&waterfallFb[dataWidth], waterfallFb, dataWidth * (waterfallHeight - 1) * sizeof(uint32_t));

            const float dataRange = waterfallMax - waterfallMin;

            for (int j = 0; j < dataWidth; j++) {
                float pixel = (std::clamp<float>(latestFFT[j], waterfallMin, waterfallMax) - waterfallMin) / dataRange;
                int id = (int)(pixel * (WATERFALL_RESOLUTION - 1));

                waterfallFb[j] = waterfallPallet[id];
            }
            waterfallUpdate = true;
        }
        else {
            zoom(latestFFT, rawFFTs);
            fftLines = 1;
        }

        // Apply smoothing if enabled
        if (fftSmoothing && latestFFT != NULL && smoothingBuf != NULL && fftLines != 0) {
            std::lock_guard<std::mutex> lck2(smoothingBufMtx);
            volk_32f_s32f_multiply_32f(latestFFT, latestFFT, fftSmoothingAlpha, dataWidth);
            volk_32f_s32f_multiply_32f(smoothingBuf, smoothingBuf, fftSmoothingBeta, dataWidth);
            volk_32f_x2_add_32f(smoothingBuf, latestFFT, smoothingBuf, dataWidth);
            memcpy(latestFFT, smoothingBuf, dataWidth * sizeof(float));
        }

        if (selectedVFO != "" && vfos.size() > 0) {
            float dummy;
            if (snrSmoothing) {
                float newSNR = 0.0f;
                calculateVFOSignalInfo(waterfallVisible ? &rawFFTs[currentFFTLine * rawFFTSize] : rawFFTs, vfos[selectedVFO], dummy, newSNR);
                selectedVFOSNR = (snrSmoothingBeta*selectedVFOSNR) + (snrSmoothingAlpha*newSNR);
            }
            else {
                calculateVFOSignalInfo(waterfallVisible ? &rawFFTs[currentFFTLine * rawFFTSize] : rawFFTs, vfos[selectedVFO], dummy, selectedVFOSNR);
            }
        }

        // If FFT hold is enabled, update it
        if (fftHold && latestFFT != NULL && latestFFTHold != NULL && fftLines != 0) {
            for (int i = 1; i < dataWidth; i++) {
                latestFFTHold[i] = std::max<float>(latestFFT[i], latestFFTHold[i] - fftHoldSpeed);
            }
        }

        buf_mtx.unlock();
    }

    void WaterFall::updatePallette(float colors[][3], int colorCount) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        for (int i = 0; i < WATERFALL_RESOLUTION; i++) {
            int lowerId = floorf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            int upperId = ceilf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            lowerId = std::clamp<int>(lowerId, 0, colorCount - 1);
            upperId = std::clamp<int>(upperId, 0, colorCount - 1);
            float ratio = (((float)i / (float)WATERFALL_RESOLUTION) * colorCount) - lowerId;
            float r = (colors[lowerId][0] * (1.0 - ratio)) + (colors[upperId][0] * (ratio));
            float g = (colors[lowerId][1] * (1.0 - ratio)) + (colors[upperId][1] * (ratio));
            float b = (colors[lowerId][2] * (1.0 - ratio)) + (colors[upperId][2] * (ratio));
            waterfallPallet[i] = ((uint32_t)255 << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
        updateWaterfallFb();
    }

    void WaterFall::updatePalletteFromArray(float* colors, int colorCount) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        for (int i = 0; i < WATERFALL_RESOLUTION; i++) {
            int lowerId = floorf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            int upperId = ceilf(((float)i / (float)WATERFALL_RESOLUTION) * colorCount);
            lowerId = std::clamp<int>(lowerId, 0, colorCount - 1);
            upperId = std::clamp<int>(upperId, 0, colorCount - 1);
            float ratio = (((float)i / (float)WATERFALL_RESOLUTION) * colorCount) - lowerId;
            float r = (colors[(lowerId * 3) + 0] * (1.0 - ratio)) + (colors[(upperId * 3) + 0] * (ratio));
            float g = (colors[(lowerId * 3) + 1] * (1.0 - ratio)) + (colors[(upperId * 3) + 1] * (ratio));
            float b = (colors[(lowerId * 3) + 2] * (1.0 - ratio)) + (colors[(upperId * 3) + 2] * (ratio));
            waterfallPallet[i] = ((uint32_t)255 << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
        updateWaterfallFb();
    }

    void WaterFall::autoRange() {
        std::lock_guard<std::recursive_mutex> lck(latestFFTMtx);
        float min = INFINITY;
        float max = -INFINITY;
        for (int i = 0; i < dataWidth; i++) {
            if (latestFFT[i] < min) {
                min = latestFFT[i];
            }
            if (latestFFT[i] > max) {
                max = latestFFT[i];
            }
        }
        fftMin = min - 5;
        fftMax = max + 5;
    }

    void WaterFall::setCenterFrequency(double freq) {
        centerFreq = freq;
        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);
        updateAllVFOs();
    }

    double WaterFall::getCenterFrequency() {
        return centerFreq;
    }

    void WaterFall::setBandwidth(double bandWidth) {
        double currentRatio = viewBandwidth / wholeBandwidth;
        wholeBandwidth = bandWidth;
        setViewBandwidth(bandWidth * currentRatio);
        for (auto const& [name, vfo] : vfos) {
            if (vfo->lowerOffset < -(bandWidth / 2)) {
                vfo->setCenterOffset(-(bandWidth / 2));
            }
            if (vfo->upperOffset > (bandWidth / 2)) {
                vfo->setCenterOffset(bandWidth / 2);
            }
        }
        updateAllVFOs();
    }

    double WaterFall::getBandwidth() {
        return wholeBandwidth;
    }

    void WaterFall::setViewBandwidth(double bandWidth) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (bandWidth == viewBandwidth) {
            return;
        }
        if (abs(viewOffset) + (bandWidth / 2.0) > wholeBandwidth / 2.0) {
            if (viewOffset < 0) {
                viewOffset = (bandWidth / 2.0) - (wholeBandwidth / 2.0);
            }
            else {
                viewOffset = (wholeBandwidth / 2.0) - (bandWidth / 2.0);
            }
        }
        viewBandwidth = bandWidth;
        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);
        range = findBestRange(bandWidth, maxHSteps);
        if (_fullUpdate) { updateWaterfallFb(); };
        updateAllVFOs();
    }

    double WaterFall::getViewBandwidth() {
        return viewBandwidth;
    }

    void WaterFall::setViewOffset(double offset) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (offset == viewOffset) {
            return;
        }
        if (offset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0)) {
            offset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
        }
        if (offset + (viewBandwidth / 2.0) > (wholeBandwidth / 2.0)) {
            offset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
        }
        viewOffset = offset;
        lowerFreq = (centerFreq + viewOffset) - (viewBandwidth / 2.0);
        upperFreq = (centerFreq + viewOffset) + (viewBandwidth / 2.0);
        if (_fullUpdate) { updateWaterfallFb(); };
        updateAllVFOs();
    }

    double WaterFall::getViewOffset() {
        return viewOffset;
    }

    void WaterFall::setFFTMin(float min) {
        fftMin = min;
        vRange = findBestRange(fftMax - fftMin, maxVSteps);
    }

    float WaterFall::getFFTMin() {
        return fftMin;
    }

    void WaterFall::setFFTMax(float max) {
        fftMax = max;
        vRange = findBestRange(fftMax - fftMin, maxVSteps);
    }

    float WaterFall::getFFTMax() {
        return fftMax;
    }

    void WaterFall::setFullWaterfallUpdate(bool fullUpdate) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        _fullUpdate = fullUpdate;
    }

    void WaterFall::setWaterfallMin(float min) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (min == waterfallMin) {
            return;
        }
        waterfallMin = min;
        if (_fullUpdate) { updateWaterfallFb(); };
    }

    float WaterFall::getWaterfallMin() {
        return waterfallMin;
    }

    void WaterFall::setWaterfallMax(float max) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        if (max == waterfallMax) {
            return;
        }
        waterfallMax = max;
        if (_fullUpdate) { updateWaterfallFb(); };
    }

    float WaterFall::getWaterfallMax() {
        return waterfallMax;
    }

    void WaterFall::updateAllVFOs(bool checkRedrawRequired) {
        for (auto const& [name, vfo] : vfos) {
            if (checkRedrawRequired && !vfo->redrawRequired) { continue; }
            vfo->updateDrawingVars(viewBandwidth, dataWidth, viewOffset, widgetPos, fftHeight);
            vfo->wfRectMin = ImVec2(vfo->rectMin.x, wfMin.y);
            vfo->wfRectMax = ImVec2(vfo->rectMax.x, wfMax.y);
            vfo->wfLineMin = ImVec2(vfo->lineMin.x, wfMin.y - 1);
            vfo->wfLineMax = ImVec2(vfo->lineMax.x, wfMax.y - 1);
            vfo->wfLbwSelMin = ImVec2(vfo->wfRectMin.x - 2, vfo->wfRectMin.y);
            vfo->wfLbwSelMax = ImVec2(vfo->wfRectMin.x + 2, vfo->wfRectMax.y);
            vfo->wfRbwSelMin = ImVec2(vfo->wfRectMax.x - 2, vfo->wfRectMin.y);
            vfo->wfRbwSelMax = ImVec2(vfo->wfRectMax.x + 2, vfo->wfRectMax.y);
            vfo->redrawRequired = false;
        }
    }

    void WaterFall::setRawFFTSize(int size) {
        std::lock_guard<std::recursive_mutex> lck(buf_mtx);
        rawFFTSize = size;
        int wfSize = std::max<int>(1, waterfallHeight);
        if (rawFFTs != NULL) {
            rawFFTs = (float*)realloc(rawFFTs, rawFFTSize * wfSize * sizeof(float));
        }
        else {
            rawFFTs = (float*)malloc(rawFFTSize * wfSize * sizeof(float));
        }
        fftLines = 0;
        memset(rawFFTs, 0, rawFFTSize * waterfallHeight * sizeof(float));
        updateWaterfallFb();
    }

    void WaterFall::setBandPlanPos(int pos) {
        bandPlanPos = pos;
    }

    void WaterFall::setFFTHold(bool hold) {
        fftHold = hold;
        if (fftHold && latestFFTHold) {
            for (int i = 0; i < dataWidth; i++) {
                latestFFTHold[i] = -1000.0;
            }
        }
    }

    void WaterFall::setFFTHoldSpeed(float speed) {
        fftHoldSpeed = speed;
    }

    void WaterFall::setFFTSmoothing(bool enabled) {
        std::lock_guard<std::mutex> lck(smoothingBufMtx);
        fftSmoothing = enabled;

        delete[] smoothingBuf;

        // If disabled, stop here
        if (!enabled) {
            smoothingBuf = NULL;
            return;
        }

        // Allocate and copy existing FFT into it
        smoothingBuf = new float[dataWidth];
        if (latestFFT) {
            std::lock_guard<std::recursive_mutex> lck2(latestFFTMtx);
            memcpy(smoothingBuf, latestFFT, dataWidth * sizeof(float));
        }
        else {
            memset(smoothingBuf, 0, dataWidth * sizeof(float));
        }
    }

    void WaterFall::setFFTSmoothingSpeed(float speed) {
        std::lock_guard<std::mutex> lck(smoothingBufMtx);
        fftSmoothingAlpha = speed;
        fftSmoothingBeta = 1.0f - speed;
    }

    void WaterFall::setSNRSmoothing(bool enabled) {
        snrSmoothing = enabled;
    }

    void WaterFall::setSNRSmoothingSpeed(float speed) {
        snrSmoothingAlpha = speed;
        snrSmoothingBeta = 1.0f - speed;
    }

    float* WaterFall::acquireLatestFFT(int& width) {
        latestFFTMtx.lock();
        if (!latestFFT) {
            latestFFTMtx.unlock();
            return NULL;
        }
        width = dataWidth;
        return latestFFT;
    }

    void WaterFall::releaseLatestFFT() {
        latestFFTMtx.unlock();
    }

    void WaterfallVFO::setOffset(double offset) {
        generalOffset = offset;
        if (reference == REF_CENTER) {
            centerOffset = offset;
            lowerOffset = offset - (bandwidth / 2.0);
            upperOffset = offset + (bandwidth / 2.0);
        }
        else if (reference == REF_LOWER) {
            lowerOffset = offset;
            centerOffset = offset + (bandwidth / 2.0);
            upperOffset = offset + bandwidth;
        }
        else if (reference == REF_UPPER) {
            upperOffset = offset;
            centerOffset = offset - (bandwidth / 2.0);
            lowerOffset = offset - bandwidth;
        }
        centerOffsetChanged = true;
        upperOffsetChanged = true;
        lowerOffsetChanged = true;
        redrawRequired = true;
    }

    void WaterfallVFO::setCenterOffset(double offset) {
        if (reference == REF_CENTER) {
            generalOffset = offset;
        }
        else if (reference == REF_LOWER) {
            generalOffset = offset - (bandwidth / 2.0);
        }
        else if (reference == REF_UPPER) {
            generalOffset = offset + (bandwidth / 2.0);
        }
        centerOffset = offset;
        lowerOffset = offset - (bandwidth / 2.0);
        upperOffset = offset + (bandwidth / 2.0);
        centerOffsetChanged = true;
        upperOffsetChanged = true;
        lowerOffsetChanged = true;
        redrawRequired = true;
    }

    void WaterfallVFO::setBandwidth(double bw) {
        if (bandwidth == bw || bw < 0) {
            return;
        }
        bandwidth = bw;
        if (reference == REF_CENTER) {
            lowerOffset = centerOffset - (bandwidth / 2.0);
            upperOffset = centerOffset + (bandwidth / 2.0);
        }
        else if (reference == REF_LOWER) {
            centerOffset = lowerOffset + (bandwidth / 2.0);
            upperOffset = lowerOffset + bandwidth;
            centerOffsetChanged = true;
        }
        else if (reference == REF_UPPER) {
            centerOffset = upperOffset - (bandwidth / 2.0);
            lowerOffset = upperOffset - bandwidth;
            centerOffsetChanged = true;
        }
        bandwidthChanged = true;
        redrawRequired = true;
    }

    void WaterfallVFO::setReference(int ref) {
        if (reference == ref || ref < 0 || ref >= _REF_COUNT) {
            return;
        }
        reference = ref;
        setOffset(generalOffset);
    }

    void WaterfallVFO::setNotchOffset(double offset) {
        notchOffset = offset;
        redrawRequired = true;
    }

    void WaterfallVFO::setNotchVisible(bool visible) {
        notchVisible = visible;
        redrawRequired = true;
    }

    void WaterfallVFO::updateDrawingVars(double viewBandwidth, float dataWidth, double viewOffset, ImVec2 widgetPos, int fftHeight) {
        int center = roundf((((centerOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));
        int left = roundf((((lowerOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));
        int right = roundf((((upperOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));
        int notch = roundf((((notchOffset + centerOffset - viewOffset) / (viewBandwidth / 2.0)) + 1.0) * ((double)dataWidth / 2.0));

        // Check weather the line is visible
        if (left >= 0 && left < dataWidth && reference == REF_LOWER) {
            lineVisible = true;
        }
        else if (center >= 0 && center < dataWidth && reference == REF_CENTER) {
            lineVisible = true;
        }
        else if (right >= 0 && right < dataWidth && reference == REF_UPPER) {
            lineVisible = true;
        }
        else {
            lineVisible = false;
        }

        // Calculate the position of the line
        if (reference == REF_LOWER) {
            lineMin = ImVec2(gui::waterfall.fftAreaMin.x + left, gui::waterfall.fftAreaMin.y);
            lineMax = ImVec2(gui::waterfall.fftAreaMin.x + left, gui::waterfall.fftAreaMax.y - 1);
        }
        else if (reference == REF_CENTER) {
            lineMin = ImVec2(gui::waterfall.fftAreaMin.x + center, gui::waterfall.fftAreaMin.y);
            lineMax = ImVec2(gui::waterfall.fftAreaMin.x + center, gui::waterfall.fftAreaMax.y - 1);
        }
        else if (reference == REF_UPPER) {
            lineMin = ImVec2(gui::waterfall.fftAreaMin.x + right, gui::waterfall.fftAreaMin.y);
            lineMax = ImVec2(gui::waterfall.fftAreaMin.x + right, gui::waterfall.fftAreaMax.y - 1);
        }

        int _left = left;
        int _right = right;
        left = std::clamp<int>(left, 0, dataWidth - 1);
        right = std::clamp<int>(right, 0, dataWidth - 1);
        leftClamped = (left != _left);
        rightClamped = (right != _right);

        rectMin = ImVec2(gui::waterfall.fftAreaMin.x + left, gui::waterfall.fftAreaMin.y + 1);
        rectMax = ImVec2(gui::waterfall.fftAreaMin.x + right + 1, gui::waterfall.fftAreaMax.y);

        float gripSize = 2.0f * style::uiScale;
        lbwSelMin = ImVec2(rectMin.x - gripSize, rectMin.y);
        lbwSelMax = ImVec2(rectMin.x + gripSize, rectMax.y);
        rbwSelMin = ImVec2(rectMax.x - gripSize, rectMin.y);
        rbwSelMax = ImVec2(rectMax.x + gripSize, rectMax.y);

        notchMin = ImVec2(gui::waterfall.fftAreaMin.x + notch - gripSize, gui::waterfall.fftAreaMin.y);
        notchMax = ImVec2(gui::waterfall.fftAreaMin.x + notch + gripSize, gui::waterfall.fftAreaMax.y - 1);
    }

    void WaterfallVFO::draw(ImGuiWindow* window, bool selected) {
        window->DrawList->AddRectFilled(rectMin, rectMax, color);
        if (lineVisible) {
            window->DrawList->AddLine(lineMin, lineMax, selected ? IM_COL32(255, 0, 0, 255) : IM_COL32(255, 255, 0, 255), style::uiScale);
        }

        if (notchVisible) {
            window->DrawList->AddRectFilled(notchMin, notchMax, IM_COL32(255, 0, 0, 127));
        }

        if (!gui::mainWindow.lockWaterfallControls && !gui::waterfall.inputHandled) {
            ImVec2 mousePos = ImGui::GetMousePos();
            if (rectMax.x - rectMin.x < 10) { return; }
            if (reference != REF_LOWER && !bandwidthLocked && !leftClamped) {
                if (IS_IN_AREA(mousePos, lbwSelMin, lbwSelMax)) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); }
                else if (IS_IN_AREA(mousePos, wfLbwSelMin, wfLbwSelMax)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            }
            if (reference != REF_UPPER && !bandwidthLocked && !rightClamped) {
                if (IS_IN_AREA(mousePos, rbwSelMin, rbwSelMax)) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); }
                else if (IS_IN_AREA(mousePos, wfRbwSelMin, wfRbwSelMax)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            }
        }
    };

    void WaterFall::showWaterfall() {
        buf_mtx.lock();
        if (rawFFTs == NULL) {
            flog::error("Null rawFFT");
        }
        waterfallVisible = true;
        onResize();
        memset(rawFFTs, 0, waterfallHeight * rawFFTSize * sizeof(float));
        updateWaterfallFb();
        buf_mtx.unlock();
    }

    void WaterFall::hideWaterfall() {
        buf_mtx.lock();
        waterfallVisible = false;
        onResize();
        buf_mtx.unlock();
    }

    void WaterFall::setFFTHeight(int height) {
        FFTAreaHeight = height;
        newFFTAreaHeight = height;
        buf_mtx.lock();
        onResize();
        buf_mtx.unlock();
    }

    int WaterFall::getFFTHeight() {
        return FFTAreaHeight;
    }

    void WaterFall::showBandplan() {
        bandplanVisible = true;
    }

    void WaterFall::hideBandplan() {
        bandplanVisible = false;
    }

    void WaterfallVFO::setSnapInterval(double interval) {
        snapInterval = interval;
    }
};
