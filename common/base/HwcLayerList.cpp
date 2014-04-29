/*
 * Copyright © 2012 Intel Corporation
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jackie Li <yaodong.li@intel.com>
 *
 */
#include <HwcTrace.h>
#include <Drm.h>
#include <HwcLayerList.h>
#include <Hwcomposer.h>
#include <GraphicBuffer.h>
#include <IDisplayDevice.h>
#include <PlaneCapabilities.h>
#include <DisplayQuery.h>

namespace android {
namespace intel {

HwcLayerList::HwcLayerList(hwc_display_contents_1_t *list, int disp)
    : mList(list),
      mLayerCount(0),
      mLayers(),
      mFBLayers(),
      mSpriteCandidates(),
      mOverlayCandidates(),
      mZOrderConfig(),
      mFrameBufferTarget(NULL),
      mDisplayIndex(disp)
{
    initialize();
}

HwcLayerList::~HwcLayerList()
{
    deinitialize();
}

bool HwcLayerList::checkSupported(int planeType, HwcLayer *hwcLayer)
{
    bool valid = false;
    hwc_layer_1_t& layer = *(hwcLayer->getLayer());

    // if layer was forced to use FB
    if (hwcLayer->getType() == HwcLayer::LAYER_FORCE_FB) {
        VTRACE("layer was forced to use HWC_FRAMEBUFFER");
        return false;
    }

    // check layer flags
    if (layer.flags & HWC_SKIP_LAYER) {
        VTRACE("plane type %d: (skip layer flag was set)", planeType);
        return false;
    }

    if (layer.handle == 0) {
        WTRACE("invalid buffer handle");
        return false;
    }

    // check usage
    if (!hwcLayer->getUsage() & GRALLOC_USAGE_HW_COMPOSER) {
        WTRACE("not a composer layer");
        return false;
    }

    // check layer transform
    valid = PlaneCapabilities::isTransformSupported(planeType, hwcLayer);
    if (!valid) {
        VTRACE("plane type %d: (bad transform)", planeType);
        return false;
    }

    // check buffer format
    valid = PlaneCapabilities::isFormatSupported(planeType, hwcLayer);
    if (!valid) {
        VTRACE("plane type %d: (bad buffer format)", planeType);
        return false;
    }

    // check buffer size
    valid = PlaneCapabilities::isSizeSupported(planeType, hwcLayer);
    if (!valid) {
        VTRACE("plane type %d: (bad buffer size)", planeType);
        return false;
    }

    // check layer blending
    valid = PlaneCapabilities::isBlendingSupported(planeType, hwcLayer);
    if (!valid) {
        VTRACE("plane type %d: (bad blending)", planeType);
        return false;
    }

    // check layer scaling
    valid = PlaneCapabilities::isScalingSupported(planeType, hwcLayer);
    if (!valid) {
        VTRACE("plane type %d: (bad scaling)", planeType);
        return false;
    }

    // TODO: check visible region?
    return true;
}

bool HwcLayerList::initialize()
{
    if (!mList || mList->numHwLayers == 0) {
        ETRACE("invalid hwc list");
        return false;
    }

    mLayerCount = (int)mList->numHwLayers;
    mLayers.setCapacity(mLayerCount);
    mFBLayers.setCapacity(mLayerCount);
    mSpriteCandidates.setCapacity(mLayerCount);
    mOverlayCandidates.setCapacity(mLayerCount);
    mZOrderConfig.setCapacity(mLayerCount);
    Hwcomposer& hwc = Hwcomposer::getInstance();

    for (int i = 0; i < mLayerCount; i++) {
        hwc_layer_1_t *layer = &mList->hwLayers[i];
        if (!layer) {
            DEINIT_AND_RETURN_FALSE("layer %d is null", i);
        }

        HwcLayer *hwcLayer = new HwcLayer(i, layer);
        if (!hwcLayer) {
            DEINIT_AND_RETURN_FALSE("failed to allocate hwc layer %d", i);
        }

        if (layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
            hwcLayer->setType(HwcLayer::LAYER_FRAMEBUFFER_TARGET);
            mFrameBufferTarget = hwcLayer;
        } else if (layer->compositionType == HWC_OVERLAY){
            // skipped layer, filtered by Display Analyzer
            hwcLayer->setType(HwcLayer::LAYER_SKIPPED);
        } else if (layer->compositionType == HWC_FORCE_FRAMEBUFFER) {
            layer->compositionType = HWC_FRAMEBUFFER;
            hwcLayer->setType(HwcLayer::LAYER_FORCE_FB);
            // add layer to FB layer list for zorder check during plane assignment
            mFBLayers.add(hwcLayer);
        } else  if (layer->compositionType == HWC_FRAMEBUFFER) {
            // by default use GPU composition
            hwcLayer->setType(HwcLayer::LAYER_FB);
            mFBLayers.add(hwcLayer);
            if (checkSupported(DisplayPlane::PLANE_SPRITE, hwcLayer)) {
                mSpriteCandidates.add(hwcLayer);
            } else if (hwc.getDisplayAnalyzer()->isOverlayAllowed() &&
                checkSupported(DisplayPlane::PLANE_OVERLAY, hwcLayer)) {
                mOverlayCandidates.add(hwcLayer);
            } else {
                // noncandidate layer
            }
        } else {
            DEINIT_AND_RETURN_FALSE("invalid composition type %d", layer->compositionType);
        }
        // add layer to layer list
        mLayers.add(hwcLayer);
    }

    if (mFrameBufferTarget == NULL) {
        ETRACE("no frame buffer target?");
        return false;
    }

#if 1
    allocatePlanesV2();
#else
    allocatePlanesV1();
#endif

    //dump();
    return true;
}

void HwcLayerList::deinitialize()
{
    if (mLayerCount == 0) {
        return;
    }

    DisplayPlaneManager *planeManager = Hwcomposer::getInstance().getPlaneManager();
    for (int i = 0; i < mLayerCount; i++) {
        HwcLayer *hwcLayer = mLayers.itemAt(i);
        if (hwcLayer) {
            DisplayPlane *plane = hwcLayer->detachPlane();
            if (plane) {
                planeManager->reclaimPlane(mDisplayIndex, *plane);
            }
        }
        delete hwcLayer;
    }

    mLayers.clear();
    mFBLayers.clear();
    mOverlayCandidates.clear();
    mSpriteCandidates.clear();
    mZOrderConfig.clear();
    mFrameBufferTarget = NULL;
    mLayerCount = 0;
}

bool HwcLayerList::allocatePlanesV1()
{
    DisplayPlaneManager *planeManager = Hwcomposer::getInstance().getPlaneManager();
    int overlayCandidates = (int)mOverlayCandidates.size();
    int spriteCandidates = (int)mSpriteCandidates.size();
    int overlayPlanes = planeManager->getFreePlanes(mDisplayIndex, DisplayPlane::PLANE_OVERLAY);
    int spritePlanes = planeManager->getFreePlanes(mDisplayIndex, DisplayPlane::PLANE_SPRITE);
    ZOrderLayer *zOverlay = NULL, *zSprite = NULL;
    //DTRACE("overlayCandidates %d, overlayPlanes %d, spriteCandidates %d, spritePlanes %d",
    //        overlayCandidates, overlayPlanes, spriteCandidates, spritePlanes);

    if (overlayPlanes > overlayCandidates)
        overlayPlanes = overlayCandidates;

    if (spritePlanes > spriteCandidates)
        spritePlanes = spriteCandidates;

    int overlayIndex(0), spriteIndex(0);
    for (overlayIndex = 0; overlayIndex < overlayPlanes; overlayIndex++) {
        zOverlay = addZOrderLayer(DisplayPlane::PLANE_OVERLAY, mOverlayCandidates[overlayIndex]);
    }

    for (spriteIndex = 0; spriteIndex < spritePlanes; spriteIndex++) {
        zSprite = addZOrderLayer(DisplayPlane::PLANE_SPRITE, mSpriteCandidates[spriteIndex]);
    }

    bool again = false;
    do {
        if (assignPrimaryPlane())
            return true;

        // try other possible candidate in lower priority
        again = false;

        if (spritePlanes && spriteIndex < spriteCandidates) {
            removeZOrderLayer(zSprite);
            zSprite = addZOrderLayer(DisplayPlane::PLANE_SPRITE, mSpriteCandidates[spriteIndex]);
            spriteIndex++;
            again = true;
            continue;
        }

        if (overlayPlanes && overlayIndex < overlayCandidates) {
            removeZOrderLayer(zOverlay);
            zOverlay = addZOrderLayer(DisplayPlane::PLANE_OVERLAY, mOverlayCandidates[overlayIndex]);
            overlayIndex++;
            again = true;
            continue;
        }
    } while (again);

    while (mZOrderConfig.size()) {
        // remove layer with the least priority
        ZOrderLayer *layer = mZOrderConfig[mZOrderConfig.size() - 1];
        removeZOrderLayer(layer);
        if (assignPrimaryPlane())
            return true;
    }

    ETRACE("no plane allocated, should never happen");
    return false;
}


bool HwcLayerList::allocatePlanesV2()
{
    return assignOverlayPlanes();
}

bool HwcLayerList::assignOverlayPlanes()
{
    int overlayCandidates = (int)mOverlayCandidates.size();
    if (overlayCandidates == 0) {
        return assignSpritePlanes();
    }

    DisplayPlaneManager *planeManager = Hwcomposer::getInstance().getPlaneManager();
    int planeNumber = planeManager->getFreePlanes(mDisplayIndex, DisplayPlane::PLANE_OVERLAY);
    if (planeNumber == 0) {
        DTRACE("no overlay plane available. candidates %d", overlayCandidates);
        return assignSpritePlanes();
    }

    if (planeNumber > overlayCandidates) {
        // assuming all overlay planes have the same capabilities, just
        // need up to number of candidates for plane assignment
        planeNumber = overlayCandidates;
    }

    for (int i = planeNumber; i >= 0; i--) {
        // assign as many overlay planes as possible
        if (assignOverlayPlanes(0, i)) {
            return true;
        }
        if (mZOrderConfig.size() != 0) {
            ETRACE("ZOrder config is not cleaned up!");
        }
    }
    return false;
}


bool HwcLayerList::assignOverlayPlanes(int index, int planeNumber)
{
    // index indicates position in mOverlayCandidates to start plane assignment
    if (planeNumber == 0) {
        return assignSpritePlanes();
    }

    int overlayCandidates = (int)mOverlayCandidates.size();
    for (int i = index; i <= overlayCandidates - planeNumber; i++) {
        ZOrderLayer *zlayer = addZOrderLayer(DisplayPlane::PLANE_OVERLAY, mOverlayCandidates[i]);
        if (assignOverlayPlanes(i + 1, planeNumber - 1)) {
            return true;
        }
        removeZOrderLayer(zlayer);
    }
    return false;
}

bool HwcLayerList::assignSpritePlanes()
{
    int spriteCandidates = (int)mSpriteCandidates.size();
    if (spriteCandidates == 0) {
        return assignPrimaryPlane();
    }

    //  number does not include primary plane
    DisplayPlaneManager *planeManager = Hwcomposer::getInstance().getPlaneManager();
    int planeNumber = planeManager->getFreePlanes(mDisplayIndex, DisplayPlane::PLANE_SPRITE);
    if (planeNumber == 0) {
        VTRACE("no sprite plane available, candidates %d", spriteCandidates);
        return assignPrimaryPlane();
    }

    if (planeNumber > spriteCandidates) {
        // assuming all sprite planes have the same capabilities, just
        // need up to number of candidates for plane assignment
        planeNumber = spriteCandidates;
    }

    for (int i = planeNumber; i >= 0; i--) {
        // assign as many sprite planes as possible
        if (assignSpritePlanes(0, i)) {
            return true;
        }

        if (mOverlayCandidates.size() == 0 && mZOrderConfig.size() != 0) {
            ETRACE("ZOrder config is not cleaned up!");
        }
    }
    return false;
}


bool HwcLayerList::assignSpritePlanes(int index, int planeNumber)
{
    if (planeNumber == 0) {
        return assignPrimaryPlane();
    }

    int spriteCandidates = (int)mSpriteCandidates.size();
    for (int i = index; i <= spriteCandidates - planeNumber; i++) {
        ZOrderLayer *zlayer = addZOrderLayer(DisplayPlane::PLANE_SPRITE, mSpriteCandidates[i]);
        if (assignSpritePlanes(i + 1, planeNumber - 1)) {
            return true;
        }
        removeZOrderLayer(zlayer);
    }
    return false;
}

bool HwcLayerList::assignPrimaryPlane()
{
    // find a sprit layer that is not candidate but has lower priority than candidates.
    HwcLayer *spriteLayer = NULL;
    for (int i = (int)mSpriteCandidates.size() - 1; i >= 0; i--) {
        if (mSpriteCandidates[i]->mPlaneCandidate)
            break;

        spriteLayer = mSpriteCandidates[i];
    }

    int candidates = (int)mZOrderConfig.size();
    int layers = (int)mFBLayers.size();
    bool ok = false;

    if (candidates == layers - 1 && spriteLayer != NULL) {
        // primary plane is configured as sprite, all sprite candidates are offloaded to display planes
        ok = assignPrimaryPlaneHelper(spriteLayer);
        if (!ok) {
            DTRACE("failed to use primary as sprite plane");
        }
    } else if (candidates == 0) {
        // none assigned, use primary plane for frame buffer target and set zorder to 0
        ok = assignPrimaryPlaneHelper(mFrameBufferTarget, 0);
        if (!ok) {
            ETRACE("failed to compose all layers to primary plane, should never happen");
        }
    } else if (candidates == layers) {
        // all assigned, primary plane may be used during ZOrder config.
        ok = attachPlanes();
        if (!ok) {
            ETRACE("failed to assign layers without primary");
        }
    } else {
        // check if the remaining planes can be composed to frame buffer target (FBT)
        // look up a legitimate Z order position to place FBT.
        for (int i = 0; i < layers && !ok; i++) {
            if (mFBLayers[i]->mPlaneCandidate) {
                continue;
            }
            if (useAsFrameBufferTarget(mFBLayers[i])) {
                ok = assignPrimaryPlaneHelper(mFrameBufferTarget, mFBLayers[i]->getZOrder());
                if (!ok) {
                    VTRACE("failed to use zorder %d for frame buffer target",
                        mFBLayers[i]->getZOrder());
                }
            }
        }
        if (!ok) {
            VTRACE("no possible zorder for frame buffer target");
        }

    }
    return ok;
}

bool HwcLayerList::assignPrimaryPlaneHelper(HwcLayer *hwcLayer, int zorder)
{
    ZOrderLayer *zlayer = addZOrderLayer(DisplayPlane::PLANE_PRIMARY, hwcLayer, zorder);
    bool ok = attachPlanes();
    if (!ok) {
        removeZOrderLayer(zlayer);
    }
    return ok;
}

bool HwcLayerList::attachPlanes()
{
    DisplayPlaneManager *planeManager = Hwcomposer::getInstance().getPlaneManager();
    if (!planeManager->isValidZOrder(mDisplayIndex, mZOrderConfig)) {
        VTRACE("invalid z order, size of config %d", mZOrderConfig.size());
        return false;
    }

    if (!planeManager->assignPlanes(mDisplayIndex, mZOrderConfig)) {
        WTRACE("failed to assign planes");
        return false;
    }

    VTRACE("============= plane assignment===================");
    for (int i = 0; i < (int)mZOrderConfig.size(); i++) {
        ZOrderLayer *zlayer = mZOrderConfig.itemAt(i);
        if (zlayer->plane == NULL || zlayer->hwcLayer == NULL) {
            ETRACE("invalid ZOrderLayer, should never happen!!");
        }

        zlayer->plane->setZOrder(i);

        if (zlayer->hwcLayer != mFrameBufferTarget) {
            zlayer->hwcLayer->setType(HwcLayer::LAYER_OVERLAY);
            // update FB layers for smart composition
            mFBLayers.remove(zlayer->hwcLayer);
        }

        zlayer->hwcLayer->attachPlane(zlayer->plane, mDisplayIndex);

        VTRACE("total %d, layer %d, type %d, index %d, zorder %d",
            mLayerCount - 1,
            zlayer->hwcLayer->getIndex(),
            zlayer->plane->getType(),
            zlayer->plane->getIndex(),
            zlayer->zorder);

        delete zlayer;
    }

    mZOrderConfig.clear();
    return true;
}

bool HwcLayerList::useAsFrameBufferTarget(HwcLayer *target)
{
    // check if zorder of target can be used as zorder of frame buffer target
    // eligible only when all noncandidate layers can be merged to the target layer:
    // 1) noncandidate layer and candidate layer below the target layer can't overlap
    // if candidate layer is on top of non candidate layer, as "noncandidate layer" needs
    // to be moved up to target layer in z order;
    // 2) noncandidate layer and candidate layers above the target layer can't overlap
    // if candidate layer is below noncandidate layer, as "noncandidate layer" needs
    // to be moved down to target layer in z order.

    int targetLayerIndex = target->getIndex();

    // check candidate and noncandidate layers below this candidate does not overlap
    for (int below = 0; below < targetLayerIndex; below++) {
        if (mFBLayers[below]->mPlaneCandidate) {
            continue;
        } else {
            // check candidate layer above this noncandidate layer does not overlap
            for (int above = below + 1; above < targetLayerIndex; above++) {
                if (mFBLayers[above]->mPlaneCandidate == false) {
                    continue;
                }
                if (hasIntersection(mFBLayers[above], mFBLayers[below])) {
                    return false;
                }
            }
        }
    }

    // check candidate and noncandidate layers above this candidate does not overlap
    for (int above = targetLayerIndex + 1; above < mLayerCount - 1; above++) {
        if (mFBLayers[above]->mPlaneCandidate) {
            continue;
        } else {
            // check candidate layer below this noncandidate layer does not overlap
            for (int below = targetLayerIndex + 1; below < above; below++) {
                if (mFBLayers[below]->mPlaneCandidate == false) {
                    continue;
                }
                if (hasIntersection(mFBLayers[above], mFBLayers[below])) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool HwcLayerList::hasIntersection(HwcLayer *la, HwcLayer *lb)
{
    hwc_layer_1_t *a = la->getLayer();
    hwc_layer_1_t *b = lb->getLayer();
    hwc_rect_t *aRect = &a->displayFrame;
    hwc_rect_t *bRect = &b->displayFrame;

    if (bRect->right <= aRect->left ||
        bRect->left >= aRect->right ||
        bRect->top >= aRect->bottom ||
        bRect->bottom <= aRect->top)
        return false;

    return true;
}

ZOrderLayer* HwcLayerList::addZOrderLayer(int type, HwcLayer *hwcLayer, int zorder)
{
    ZOrderLayer *layer = new ZOrderLayer;
    layer->planeType = type;
    layer->hwcLayer = hwcLayer;
    layer->zorder = (zorder != -1) ? zorder : hwcLayer->getZOrder();
    layer->plane = NULL;

    if (hwcLayer->mPlaneCandidate) {
        ETRACE("plane is candidate!, order = %d", zorder);
    }

    hwcLayer->mPlaneCandidate = true;

    if ((int)mZOrderConfig.indexOf(layer) >= 0) {
        ETRACE("layer exists!");
    }

    mZOrderConfig.add(layer);
    return layer;
}

void HwcLayerList::removeZOrderLayer(ZOrderLayer *layer)
{
    if ((int)mZOrderConfig.indexOf(layer) < 0) {
        ETRACE("layer does not exist!");
    }

    mZOrderConfig.remove(layer);

    if (layer->hwcLayer->mPlaneCandidate == false) {
        ETRACE("plane is not candidate!, order %d", layer->zorder);
    }
    layer->hwcLayer->mPlaneCandidate = false;
    delete layer;
}

void HwcLayerList::setupSmartComposition()
{
    uint32_t compositionType = HWC_OVERLAY;
    HwcLayer *hwcLayer = NULL;

    // setup smart composition only there's no update on all FB layers
    for (size_t i = 0; i < mFBLayers.size(); i++) {
        hwcLayer = mFBLayers.itemAt(i);
        if (hwcLayer->isUpdated()) {
            compositionType = HWC_FRAMEBUFFER;
        }
    }

    VTRACE("smart composition enabled %s",
           (compositionType == HWC_OVERLAY) ? "TRUE" : "FALSE");
    for (size_t i = 0; i < mFBLayers.size(); i++) {
        hwcLayer = mFBLayers.itemAt(i);
        switch (hwcLayer->getType()) {
        case HwcLayer::LAYER_FB:
        case HwcLayer::LAYER_FORCE_FB:
            hwcLayer->setCompositionType(compositionType);
            break;
        default:
            ETRACE("Invalid layer type %d", hwcLayer->getType());
            break;
        }
    }
}

bool HwcLayerList::update(hwc_display_contents_1_t *list)
{
    bool ret;

    // basic check to make sure the consistance
    if (!list) {
        ETRACE("null layer list");
        return false;
    }

    if ((int)list->numHwLayers != mLayerCount) {
        ETRACE("layer count doesn't match (%d, %d)", list->numHwLayers, mLayerCount);
        return false;
    }

    // update list
    mList = list;

    // update all layers, call each layer's update()
    for (int i = 0; i < mLayerCount; i++) {
        HwcLayer *hwcLayer = mLayers.itemAt(i);
        if (!hwcLayer) {
            ETRACE("no HWC layer for layer %d", i);
            continue;
        }

        hwcLayer->update(&list->hwLayers[i]);
    }

    setupSmartComposition();
    return true;
}

DisplayPlane* HwcLayerList::getPlane(uint32_t index) const
{
    HwcLayer *hwcLayer;

    if (index >= mLayers.size()) {
        ETRACE("invalid layer index %d", index);
        return 0;
    }

    hwcLayer = mLayers.itemAt(index);
    if ((hwcLayer->getType() == HwcLayer::LAYER_FB) ||
        (hwcLayer->getType() == HwcLayer::LAYER_FORCE_FB) ||
        (hwcLayer->getType() == HwcLayer::LAYER_SKIPPED)) {
        return 0;
    }

    if (hwcLayer->getHandle() == 0) {
        WTRACE("plane is attached with invalid handle");
        return 0;
    }

    return hwcLayer->getPlane();
}

void HwcLayerList::postFlip()
{
    for (size_t i = 0; i < mLayers.size(); i++) {
        HwcLayer *hwcLayer = mLayers.itemAt(i);
        hwcLayer->postFlip();
    }
}

void HwcLayerList::dump(Dump& d)
{
    d.append("Layer list: (number of layers %d):\n", mLayers.size());
    d.append(" LAYER |          TYPE          |   PLANE  | INDEX | Z Order \n");
    d.append("-------+------------------------+----------------------------\n");
    for (size_t i = 0; i < mLayers.size(); i++) {
        HwcLayer *hwcLayer = mLayers.itemAt(i);
        DisplayPlane *plane;
        int planeIndex = -1;
        int zorder = -1;
        const char *type = "HWC_FB";
        const char *planeType = "N/A";

        if (hwcLayer) {
            switch (hwcLayer->getType()) {
            case HwcLayer::LAYER_FB:
            case HwcLayer::LAYER_FORCE_FB:
                type = "HWC_FB";
                break;
            case HwcLayer::LAYER_OVERLAY:
            case HwcLayer::LAYER_SKIPPED:
                type = "HWC_OVERLAY";
                break;
            case HwcLayer::LAYER_FRAMEBUFFER_TARGET:
                type = "HWC_FRAMEBUFFER_TARGET";
                break;
            default:
                type = "Unknown";
            }

            plane = hwcLayer->getPlane();
            if (plane) {
                planeIndex = plane->getIndex();
                zorder = plane->getZOrder();
                switch (plane->getType()) {
                case DisplayPlane::PLANE_OVERLAY:
                    planeType = "OVERLAY";
                    break;
                case DisplayPlane::PLANE_SPRITE:
                    planeType = "SPRITE";
                    break;
                case DisplayPlane::PLANE_PRIMARY:
                    planeType = "PRIMARY";
                    break;
                default:
                    planeType = "Unknown";
                }
            }

            d.append("  %2d   | %22s | %8s | %3D   | %3D \n",
                     i, type, planeType, planeIndex, zorder);
        }
    }
}


void HwcLayerList::dump()
{
    static char const* compositionTypeName[] = {
        "GLES",
        "HWC",
        "BG",
        "FBT",
        "N/A"};

    static char const* planeTypeName[] = {
        "SPRITE",
        "OVERLAY",
        "PRIMARY",
        "UNKNOWN"};

    DTRACE(" numHwLayers = %u, flags = %08x", mList->numHwLayers, mList->flags);

    DTRACE(" type |  handle  | hints | flags | tr | blend | alpha |  format  |           source crop             |            frame          | index | zorder |  plane  ");
    DTRACE("------+----------+-------+-------+----+-------+-------+----------+-----------------------------------+---------------------------+-------+--------+---------");


    for (int i = 0 ; i < mLayerCount ; i++) {
        const hwc_layer_1_t&l = mList->hwLayers[i];
        DisplayPlane *plane = mLayers[i]->getPlane();
        int planeIndex = -1;
        int zorder = -1;
        const char *planeType = "N/A";
        if (plane) {
            planeIndex = plane->getIndex();
            zorder = plane->getZOrder();
            planeType = planeTypeName[plane->getType()];
        }

        DTRACE(
            " %4s | %8x | %5x | %5x | %2x | %5x | %5x | %8x | [%7.1f,%7.1f,%7.1f,%7.1f] | [%5d,%5d,%5d,%5d] | %5d | %6d | %7s ",
            compositionTypeName[l.compositionType],
            mLayers[i]->getHandle(), l.hints, l.flags, l.transform, l.blending, l.planeAlpha, mLayers[i]->getFormat(),
            l.sourceCropf.left, l.sourceCropf.top, l.sourceCropf.right, l.sourceCropf.bottom,
            l.displayFrame.left, l.displayFrame.top, l.displayFrame.right, l.displayFrame.bottom,
            planeIndex, zorder, planeType);
    }

}


} // namespace intel
} // namespace android
