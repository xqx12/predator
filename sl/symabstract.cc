/*
 * Copyright (C) 2009-2010 Kamil Dudka <kdudka@redhat.com>
 * Copyright (C) 2010 Petr Peringer, FIT
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "symabstract.hh"

#include <cl/cl_msg.hh>
#include <cl/storage.hh>

#include "util.hh"

#include <stack>

#include <boost/foreach.hpp>
#include <boost/tuple/tuple.hpp>

#ifndef SE_DISABLE_DLS
#   define SE_DISABLE_DLS 0
#endif

#ifndef SE_DISABLE_SLS
#   define SE_DISABLE_SLS 0
#endif

/// common configuration template for abstraction triggering
struct AbstractionThreshold {
    unsigned sparePrefix;
    unsigned innerSegLen;
    unsigned spareSuffix;
};

/// abstraction trigger threshold for SLS
static struct AbstractionThreshold slsThreshold = {
    /* sparePrefix */ 1,
    /* innerSegLen */ 1,
    /* spareSuffix */ 0
};

/// abstraction trigger threshold for DLS
static struct AbstractionThreshold dlsThreshold = {
    /* sparePrefix */ 0,
    /* innerSegLen */ 1,
    /* spareSuffix */ 1
};

typedef std::pair<TObjId, TObjId> TObjPair;

// helper template for traverseSubObjs()
template <class TItem> struct TraverseSubObjsHelper { };

// specialisation for TObjId, which means basic implementation of the traversal
template <> struct TraverseSubObjsHelper<TObjId> {
    static const struct cl_type* getItemClt(const SymHeap &sh, TObjId obj) {
        return sh.objType(obj);
    }
    static TObjId getNextItem(const SymHeap &sh, TObjId obj, int nth) {
        return sh.subObj(obj, nth);
    }
};

// specialisation suitable for traversing two composite objects simultaneously
template <> struct TraverseSubObjsHelper<TObjPair> {
    static const struct cl_type* getItemClt(const SymHeap &sh, TObjPair item) {
        const struct cl_type *clt1 = sh.objType(item.first);
        const struct cl_type *clt2 = sh.objType(item.second);
        if (clt1 != clt2)
            TRAP;

        return clt1;
    }
    static TObjPair getNextItem(const SymHeap &sh, TObjPair item, int nth) {
        item.first  = sh.subObj(item.first,  nth);
        item.second = sh.subObj(item.second, nth);
        return item;
    }
};

// take the given visitor through a composite object (or whatever you pass in)
template <class THeap, typename TVisitor, class TItem = TObjId>
bool /* complete */ traverseSubObjs(THeap &sh, TItem item, TVisitor visitor) {
    std::stack<TItem> todo;
    todo.push(item);
    while (!todo.empty()) {
        item = todo.top();
        todo.pop();

        typedef TraverseSubObjsHelper<TItem> THelper;
        const struct cl_type *clt = THelper::getItemClt(sh, item);
        if (!clt || clt->code != CL_TYPE_STRUCT)
            TRAP;

        for (int i = 0; i < clt->item_cnt; ++i) {
            const TItem next = THelper::getNextItem(sh, item, i);
            if (!/* continue */visitor(sh, next))
                return false;

            const struct cl_type *subClt = THelper::getItemClt(sh, next);
            if (subClt && subClt->code == CL_TYPE_STRUCT)
                todo.push(next);
        }
    }

    // the traversal is done, without any interruption by visitor
    return true;
}

namespace {

bool doesAnyonePointToInsideVisitor(const SymHeap &sh, TObjId sub) {
    const TValueId subAddr = sh.placedAt(sub);
    return /* continue */ !sh.usedByCount(subAddr);
}

bool doesAnyonePointToInside(const SymHeap &sh, TObjId obj) {
    return !traverseSubObjs(sh, obj, doesAnyonePointToInsideVisitor);
}

bool abstractNonMatchingValuesVisitor(SymHeap &sh, TObjPair item) {
    const TObjId dst = item.second;
    const TValueId valSrc = sh.valueOf(item.first);
    const TValueId valDst = sh.valueOf(dst);
    bool eq;
    if (sh.proveEq(&eq, valSrc, valDst) && eq)
        // values are equal
        return /* continue */ true;

    // attempt to dig some type-info for the new unknown value
    const struct cl_type *clt = sh.valType(valSrc);
    const struct cl_type *cltDst = sh.valType(valDst);
    if (!clt)
        clt = cltDst;
    else if (cltDst && cltDst != clt)
        // should be safe to ignore
        TRAP;

    // create a new unknown value as a placeholder
    const TValueId valNew = sh.valCreateUnknown(UV_UNKNOWN, clt);

    // FIXME: A virtual junk may be introduced at this point!  The junk is not
    // anyhow reported to user, but causes the annoying warnings about dangling
    // root objects.  We should probably treat it as regular (false?) alarm,
    // utilize SymHeapProcessor to collect it and properly report.  However it
    // requires to pull-in location info, backtrace and the like.  Luckily, the
    // junk is not going to survive next run of symcut anyway, so it should not
    // shoot down the analysis completely.
    sh.objSetValue(dst, valNew);
    return /* continue */ true;
}

// when abstracting an object, we need to abstract all non-matching values in
void abstractNonMatchingValues(SymHeap &sh, TObjId src, TObjId dst) {
    const EObjKind kind = sh.objKind(dst);
    if (OK_CONCRETE == kind)
        // invalid call of abstractNonMatchingValues()
        TRAP;

    // wait, first preserve the value of binder and peer
    const TObjId objBind = subObjByChain(sh, dst, sh.objBinderField(dst));
    const TValueId valBind = sh.valueOf(objBind);
    TObjId   objPeer = OBJ_INVALID;
    TValueId valPeer = VAL_INVALID;
    if (OK_DLS == kind) {
        objPeer = subObjByChain(sh, dst, sh.objPeerField(dst));
        valPeer = sh.valueOf(objPeer);
    }

    // traverse all sub-objects
    const TObjPair item(src, dst);
    traverseSubObjs(sh, item, abstractNonMatchingValuesVisitor);

    // now restore the possibly smashed value of binder and peer
    sh.objSetValue(objBind, valBind);
    if (OK_DLS == kind)
        sh.objSetValue(objPeer, valPeer);
}

void abstractNonMatchingValuesBidir(SymHeap &sh, TObjId o1, TObjId o2) {
    // TODO: extend the visitor to do both in one pass
    abstractNonMatchingValues(sh, o1, o2);
    abstractNonMatchingValues(sh, o2, o1);
}

void objReplace(SymHeap &sh, TObjId oldObj, TObjId newObj) {
    if (OBJ_INVALID != sh.objParent(oldObj)
            || OBJ_INVALID != sh.objParent(newObj))
        // attempt to replace a sub-object
        TRAP;

    // resolve object addresses
    const TValueId oldAddr = sh.placedAt(oldObj);
    const TValueId newAddr = sh.placedAt(newObj);
    if (oldAddr <= 0 || newAddr <= 0)
        TRAP;

    // update all references
    sh.valReplace(oldAddr, newAddr);

    // now destroy the old object
    sh.objDestroy(oldObj);
}

void skipObj(const SymHeap &sh, TObjId *pObj, TFieldIdxChain icNext) {
    const TObjId objPtrNext = subObjByChain(sh, *pObj, icNext);
    const TValueId valNext = sh.valueOf(objPtrNext);
    const TObjId objNext = sh.pointsTo(valNext);
    if (OBJ_INVALID == objNext)
        TRAP;

    // move to the next object
    *pObj = objNext;
}

TObjId dlSegPeer(const SymHeap &sh, TObjId dls) {
    if (OK_DLS != sh.objKind(dls))
        // invalid call of dlsPeer()
        TRAP;

    TObjId peer = dls;
    skipObj(sh, &peer, sh.objPeerField(dls));
    return peer;
}

class ProbeVisitor {
    private:
        TValueId                addr_;
        const struct cl_type    *clt_;
        unsigned                arrity_;

    public:
        ProbeVisitor(const SymHeap &sh, TObjId root, EObjKind kind) {
            addr_ = sh.placedAt(root);
            clt_  = sh.objType(root);
            if (!addr_ || !clt_ || CL_TYPE_STRUCT != clt_->code)
                TRAP;

            arrity_ = static_cast<unsigned>(kind);
            if (!arrity_)
                TRAP;
        }

    bool operator()(const SymHeap &sh, TObjId obj) const {
        const TValueId valNext = sh.valueOf(obj);
        if (valNext <= 0 || valNext == addr_ || sh.valType(valNext) != clt_)
            return /* continue */ true;

        const EUnknownValue code = sh.valGetUnknown(valNext);
        switch (code) {
            case UV_KNOWN:
            case UV_ABSTRACT:
                // only known objects can be chained
                break;

            default:
                return /* continue */ true;
        }

        const TObjId target = sh.pointsTo(valNext);
        const TValueId targetAddr = sh.placedAt(target);
        if (targetAddr <= 0)
            TRAP;

        if (sh.cVar(0, obj))
            // a list segment through non-heap objects basically makes no sense
            return /* continue */ true;

        if (sh.usedByCount(targetAddr) != arrity_)
            return /* continue */ true;

        return doesAnyonePointToInside(sh, target);
    }
};

bool probe(SymHeap &sh, TObjId obj, EObjKind kind) {
    if (doesAnyonePointToInside(sh, obj))
        return false;

    const ProbeVisitor visitor(sh, obj, kind);
    return !traverseSubObjs(sh, obj, visitor);
}

// TODO: hook this somehow on the existing visitor infrastructure in order
//       to avoid code duplicity ... challenge? ;-)
//
// NOTE: we have basically the same code in SymHeapPlotter::Private::digObj()
template <class TDst>
void digAnyListSelectors(TDst &dst, const SymHeap &sh, TObjId obj,
                         EObjKind kind)
{
    const ProbeVisitor visitor(sh, obj, kind);
    TFieldIdxChain ic;

    typedef boost::tuple<TObjId, int /* nth */, bool /* last */> TStackItem;
    std::stack<TStackItem> todo;
    todo.push(TStackItem(obj, -1, false));
    while (!todo.empty()) {
        bool last;
        int nth;
        boost::tie(obj, nth, last) = todo.top();
        todo.pop();

        const struct cl_type *clt = sh.objType(obj);
        if (clt && clt->code == CL_TYPE_STRUCT) {
            if (-1 != nth)
                // nest into structure
                ic.push_back(nth);

            for (int i = 0; i < clt->item_cnt; ++i) {
                const TObjId sub = sh.subObj(obj, i);
                if (!visitor(sh, sub)) {
                    // great, we have a candidate
                    ic.push_back(i);
                    dst.push_back(ic);
                    ic.pop_back();
                }

                const struct cl_type *subClt = sh.objType(sub);
                if (subClt && subClt->code == CL_TYPE_STRUCT)
                    todo.push(TStackItem(sub, i, /* last */ (0 == i)));
            }
        }

        if (last)
            // leave the structure
            ic.pop_back();
    }
}

unsigned /* len */ discoverSeg(const SymHeap &sh, TObjId entry, EObjKind kind,
                               TFieldIdxChain icBind,
                               TFieldIdxChain icPeer = TFieldIdxChain())
{
    int dlSegsOnPath = 0;

    // we use std::set to avoid an infinite loop
    TObjId obj = entry;
    std::set<TObjId> path;
    while (!hasKey(path, obj)) {
        path.insert(obj);

        const EObjKind kindEncountered = sh.objKind(obj);
        if (OK_DLS == kindEncountered) {
            // we've hit an already existing DLS on path, let's handle it such
            if (OK_DLS != kind)
                // arrity vs. kind mismatch
                TRAP;

            // check selectors
            const TFieldIdxChain icPeerEncountered = sh.objPeerField(obj);
            if (icPeerEncountered != icBind && icPeerEncountered != icPeer)
                // completely incompatible DLS, it gives us no go
                break;

            // jump to peer
            skipObj(sh, &obj, sh.objPeerField(obj));
            if (hasKey(path, obj))
                // we came from the wrong side this time
                break;

            path.insert(obj);
            dlSegsOnPath++;
        }

        const TObjId objPtrNext = subObjByChain(sh, obj, icBind);
        const ProbeVisitor visitor(sh, obj, kind);
        if (visitor(sh, objPtrNext))
            // we can't go further
            break;

        const TValueId valNext = sh.valueOf(objPtrNext);
        const TObjId objNext = sh.pointsTo(valNext);
        if (objNext <= 0)
            // there is no valid next object
            break;

        if (OK_DLS == kind) {
            // check the back-link
            const TValueId addrSelf = sh.placedAt(obj);
            const TObjId objBackLink = subObjByChain(sh, objNext, icPeer);
            const TValueId valBackLink = sh.valueOf(objBackLink);
            if (valBackLink != addrSelf)
                // inappropriate back-link
                break;
        }

        obj = objNext;
    }

    // if there is at least one DLS on the path, we demand that the path begins
    // with a DLS;  otherwise we just ignore the path and wait for a better one
    if (dlSegsOnPath && OK_DLS != sh.objKind(entry))
        return /* not found */ 0;

    // path consisting of N nodes has N-1 edges
    const unsigned rawPathLen = path.size() - 1;

    // each DLS consists of two nodes
    return rawPathLen - dlSegsOnPath;
}

// TODO: merge somehow the following code directly into discoverAllSegments()
template <class TSelectorList>
unsigned discoverAllDlls(const SymHeap              &sh,
                         const TObjId               obj,
                         const TSelectorList        &selectors,
                         TFieldIdxChain             *icNext,
                         TFieldIdxChain             *icPrev)
{
    const int cnt = selectors.size();
    if (cnt < 2) {
        CL_DEBUG("<-- not enough selectors for OK_DLS");
        return /* found nothing */ 0;
    }

    unsigned bestLen = 0, bestNext, bestPrev;

    // if (2 < cnt), try all possible combinations
    // NOTE: This may take some time...
    for (int next = 0; next < cnt; ++next) {
        for (int prev = 0; prev < cnt; ++prev) {
            if (next == prev)
                // we demand on two distinct selectors for a DLL
                continue;

            const unsigned len  = discoverSeg(sh, obj, OK_DLS,
                                              selectors[next],
                                              selectors[prev]);
            if (!len)
                continue;

            CL_DEBUG("--- found DLS of length " << len);
            if (bestLen < len) {
                bestLen = len;
                bestNext = next;
                bestPrev = prev;
            }
        }
    }

    if (!bestLen) {
        CL_DEBUG("<--- no DLS found");
        return /* not found */ 0;
    }

    // something found
    *icNext = selectors[bestNext];
    *icPrev = selectors[bestPrev];
    return bestLen;
}

template <class TSelectorList>
unsigned discoverAllSegments(const SymHeap          &sh,
                             const TObjId           obj,
                             const EObjKind         kind,
                             const TSelectorList    &selectors,
                             TFieldIdxChain         *icNext,
                             TFieldIdxChain         *icPrev)
{
    const unsigned cnt = selectors.size();
    CL_DEBUG("--- found " << cnt << " list selector candidate(s)");
    if (!cnt)
        TRAP;

    switch (kind) {
        case OK_CONCRETE:
            // ivalid call of discoverAllSegments()
            TRAP;

        case OK_SLS:
            break;

        case OK_DLS:
            return discoverAllDlls(sh, obj, selectors, icNext, icPrev);
    }

    // choose the best selectors for SLS
    unsigned idxBest;
    unsigned slsBestLength = 0;
    for (unsigned i = 0; i < cnt; ++i) {
        const unsigned len = discoverSeg(sh, obj, OK_SLS, selectors[i]);
        if (!len)
            continue;

        CL_DEBUG("--- found SLS of length " << len);
        if (slsBestLength < len) {
            slsBestLength = len;
            idxBest = i;
        }
    }

    if (!slsBestLength) {
        CL_DEBUG("<-- no SLS found");
        return /* not found */ 0;
    }

    // something found
    *icNext = selectors[idxBest];
    return slsBestLength;
}

void ensureSlSeg(SymHeap &sh, TObjId obj, TFieldIdxChain icBind) {
    const EObjKind kind = sh.objKind(obj);
    switch (kind) {
        case OK_SLS:
            // already abstract, check binder
            if (sh.objBinderField(obj) == icBind)
                // all OK
                return;
            // fall through!

        case OK_DLS:
            TRAP;
            // fall through!

        case OK_CONCRETE:
            break;
    }

    // abstract a concrete object
    sh.objAbstract(obj, OK_SLS, icBind);

    // we're constructing the abstract object from a concrete one --> it
    // implies non-empty LS at this point
    const TValueId addr = sh.placedAt(obj);
    const TObjId objNextPtr = subObjByChain(sh, obj, icBind);
    const TValueId valNext = sh.valueOf(objNextPtr);
    if (addr <= 0 || valNext < /* we allow VAL_NULL here */ 0)
        TRAP;
    sh.addNeq(addr, valNext);
}

void slSegAbstractionStep(SymHeap &sh, TObjId *pObj, TFieldIdxChain icNext) {
    const TObjId objPtrNext = subObjByChain(sh, *pObj, icNext);
    const TValueId valNext = sh.valueOf(objPtrNext);
    if (valNext <= 0 || 1 != sh.usedByCount(valNext))
        // this looks like a failure of discoverSeg()
        TRAP;

    // make sure the next object is abstract
    const TObjId objNext = sh.pointsTo(valNext);
    ensureSlSeg(sh, objNext, icNext);
    if (OK_SLS != sh.objKind(objNext))
        TRAP;

    // replace self by the next object
    // FIXME: should be abstractNonMatchingValuesBidir()?
    abstractNonMatchingValues(sh, *pObj, objNext);
    objReplace(sh, *pObj, objNext);

    // move to the next object
    *pObj = objNext;
}

void dlsStoreCrossNeq(SymHeap &sh, TObjId obj, TObjId peer) {
    // dig the value before
    const TFieldIdxChain icBindPrev = sh.objBinderField(obj);
    const TObjId ptrPrev = subObjByChain(sh, obj, icBindPrev);
    const TValueId valPrev = sh.valueOf(ptrPrev);

    // dig the value after
    const TFieldIdxChain icBindNext = sh.objBinderField(peer);
    const TObjId ptrNext = subObjByChain(sh, peer, icBindNext);
    const TValueId valNext = sh.valueOf(ptrNext);

    // define a Neq predicate among them
    sh.addNeq(valPrev, valNext);
}

void dlSegCreate(SymHeap &sh, TObjId o1, TObjId o2,
                 TFieldIdxChain icNext, TFieldIdxChain icPrev)
{
    sh.objAbstract(o1, OK_DLS, icPrev, icNext);
    sh.objAbstract(o2, OK_DLS, icNext, icPrev);

    // introduce some UV_UNKNOWN values if necessary
    abstractNonMatchingValuesBidir(sh, o1, o2);

    // a just created DLS is said to be non-empty
    dlsStoreCrossNeq(sh, o1, o2);
}

void dlSegGoblle(SymHeap &sh, TObjId dls, TObjId var, bool backward) {
    if (OK_DLS != sh.objKind(dls) || OK_CONCRETE != sh.objKind(var))
        // invalid call of dlSegGoblle()
        TRAP;

    if (!backward)
        // jump to peer
        skipObj(sh, &dls, sh.objPeerField(dls));

    // introduce some UV_UNKNOWN values if necessary
    abstractNonMatchingValues(sh, var, dls);

    if (backward)
        // not implemented yet
        TRAP;

    // store the pointer DLS -> VAR
    const TFieldIdxChain icBind = sh.objBinderField(dls);
    const TObjId dlsNextPtr = subObjByChain(sh, dls, icBind);
    const TObjId varNextPtr = subObjByChain(sh, var, icBind);
    sh.objSetValue(dlsNextPtr, sh.valueOf(varNextPtr));

    // replace VAR by DLS
    objReplace(sh, var, dls);
}

void dlSegMerge(SymHeap &sh, TObjId seg1, TObjId seg2) {
    const TObjId peer1 = dlSegPeer(sh, seg1);
    const TObjId peer2 = dlSegPeer(sh, seg2);

    // introduce some UV_UNKNOWN values if necessary
    abstractNonMatchingValuesBidir(sh, seg1, seg2);
    abstractNonMatchingValuesBidir(sh, peer1, peer2);

    // FIXME: handle Neq predicates properly

    objReplace(sh, seg1, seg2);
    objReplace(sh, peer1, peer2);
}

void dlSegAbstractionStep(SymHeap &sh, TObjId *pObj, TFieldIdxChain icNext,
                          TFieldIdxChain icPrev)
{
    // the first object is clear
    const TObjId o1 = *pObj;

    // we'll find the next one later on
    TObjId o2 = o1;

    EObjKind kind = sh.objKind(o1);
    switch (kind) {
        case OK_SLS:
            // *** discoverSeg() failure detected ***
            TRAP;

        case OK_DLS:
            // jump to peer
            skipObj(sh, &o2, sh.objPeerField(o2));

            // jump to next object
            skipObj(sh, &o2, sh.objBinderField(o2));
            if (OK_CONCRETE == sh.objKind(o2)) {
                // DLS + VAR
                dlSegGoblle(sh, o1, o2, /* backward */ false);
                return;
            }

            // DLS + DLS
            dlSegMerge(sh, o1, o2);
            return;

        case OK_CONCRETE:
            skipObj(sh, &o2, icNext);
            if (OK_CONCRETE == sh.objKind(o2)) {
                // VAR + VAR
                dlSegCreate(sh, o1, o2, icNext, icPrev);
                return;
            }

            // VAR + DLS
            dlSegGoblle(sh, o2, o1, /* backward */ true);
            *pObj = o2;
            return;
    }
#if 0
    // abstract the next two objects, while crossing their selectors
    const bool c1 = ensureAbstract(sh, objNext,     OK_DLS, icPrev, icNext);
    const bool c2 = ensureAbstract(sh, objNextNext, OK_DLS, icNext, icPrev);

    // introduce some UV_UNKNOWN values if necessary
    abstractNonMatchingValues(sh, *pObj, objNext);

    // make sure, there are no inconsistencies among the two parts of DLS
    // FIXME: Is it always necessary?
    abstractNonMatchingValues(sh, objNext, objNextNext);
    abstractNonMatchingValues(sh, objNextNext, objNext);

    // preserve back-link
    const TValueId valBackLink = sh.valueOf(subObjByChain(sh, *pObj, icPrev));
    const TObjId objNextBackLink = subObjByChain(sh, objNext, icPrev);
    sh.objSetValue(objNextBackLink, valBackLink);

    // now materialize the DLS from two objects by a cross-link
    const TObjId objNextPeer     = subObjByChain(sh, objNext,     icNext);
    const TObjId objNextNextPeer = subObjByChain(sh, objNextNext, icPrev);
    sh.objSetValue(objNextPeer,     valNextNext);
    sh.objSetValue(objNextNextPeer, valNext    );

    // if both objects were concrete, DLS is said to be non-empty
    if (c1 && c2)
        // FIXME: the condition above is far from complete
        storeDlsCrossNeq(sh, objNext, objNextNext);

    // consume the given object and move to another one
    objReplace(sh, *pObj, objNext);
    *pObj = objNext;
#endif
}

bool considerSegAbstraction(SymHeap &sh, TObjId obj, EObjKind kind,
                            TFieldIdxChain icNext, TFieldIdxChain icPrev,
                            unsigned lenTotal)
{
    AbstractionThreshold at;
    switch (kind) {
        case OK_CONCRETE:
            // invalid call of considerSegAbstraction()
            TRAP;

        case OK_SLS:
            at = slsThreshold;
            break;

        case OK_DLS:
            at = dlsThreshold;
            break;
    }

    // check the threshold
    static const unsigned threshold = at.sparePrefix + at.innerSegLen
                                    + at.spareSuffix;

    if (lenTotal < threshold) {
        CL_DEBUG("<-- length of the longest segment (" << lenTotal
                << ") is under the threshold (" << threshold << ")");
        return false;
    }

    // handle sparePrefix/spareSuffix
    const int len = lenTotal - at.sparePrefix - at.spareSuffix;
    for (int i = 0; i < static_cast<int>(at.sparePrefix); ++i)
        skipObj(sh, &obj, icNext);

    if (OK_SLS == kind) {
        // perform SLS abstraction!
        for (int i = 0; i < len; ++i)
            slSegAbstractionStep(sh, &obj, icNext);

        CL_DEBUG("AAA successfully abstracted SLS");
        return true;
    }
    // assume OK_DLS (see the switch above)

    // perform DLS abstraction!
    for (int i = 0; i < len; ++i)
        dlSegAbstractionStep(sh, &obj, icNext, icPrev);

    CL_DEBUG("AAA successfully abstracted DLS");
    return true;
}

template <class TCont>
bool considerAbstraction(SymHeap &sh, EObjKind kind, TCont entries) {
    switch (kind) {
        case OK_CONCRETE:
            // invalid call of considerAbstraction()
            TRAP;

        case OK_SLS:
            CL_DEBUG("--> considering SLS abstraction...");
            break;

        case OK_DLS:
            CL_DEBUG("--> considering DLS abstraction...");
            break;
    }

    // go through all candidates and find the best possible abstraction
    std::vector<TFieldIdxChain> bestSelectors;
    TFieldIdxChain bestNext, bestPrev;
    TObjId bestEntry;
    unsigned bestLen = 0;
    BOOST_FOREACH(const TObjId obj, entries) {
        // gather suitable selectors
        std::vector<TFieldIdxChain> selectors;
        digAnyListSelectors(selectors, sh, obj, kind);

        // run the LS discovering process
        TFieldIdxChain icNext, icPrev;
        const unsigned len = discoverAllSegments(sh, obj, kind, selectors,
                                                 &icNext, &icPrev);

        if (len <= bestLen)
            continue;

        bestLen         = len;
        bestEntry       = obj;
        bestSelectors   = selectors;
        bestNext        = icNext;
        bestPrev        = icPrev;
    }
    if (!bestLen)
        // nothing found
        return false;

    // consider abstraction threshold and trigger the abstraction eventually
    return considerSegAbstraction(sh, bestEntry, kind, bestNext, bestPrev,
                                  bestLen);
}

bool abstractIfNeededLoop(SymHeap &sh) {
    SymHeapCore::TContObj slSegEntries;
    SymHeapCore::TContObj dlSegEntries;

    // collect all possible SLS/DLS entries
    SymHeapCore::TContObj roots;
    sh.gatherRootObjs(roots);
    BOOST_FOREACH(const TObjId obj, roots) {
        if (sh.cVar(0, obj))
            // skip static/automatic objects
            continue;

        const TValueId addr = sh.placedAt(obj);
        if (VAL_INVALID ==addr)
            continue;

        const unsigned uses = sh.usedByCount(addr);
        switch (uses) {
            case 0:
                CL_WARN("abstractIfNeededLoop() encountered an unused root "
                        "object #" << obj);
                // fall through!

            default:
                continue;

            case 1:
#if !SE_DISABLE_SLS
                if (probe(sh, obj, OK_SLS))
                    // a candidate for SLS entry
                    slSegEntries.push_back(obj);
#endif
                break;

            case 2:
#if !SE_DISABLE_DLS
                if (probe(sh, obj, OK_DLS))
                    // a candidate for DLS entry
                    dlSegEntries.push_back(obj);
#endif
                break;
        }
    }

    // TODO: check if the order of following two steps is anyhow important
    if (!slSegEntries.empty() && considerAbstraction(sh, OK_SLS, slSegEntries))
        return true;

    if (!dlSegEntries.empty() && considerAbstraction(sh, OK_DLS, dlSegEntries))
        return true;

    // no hit
    return false;
}

} // namespace

void abstractIfNeeded(SymHeap &sh) {
#if SE_DISABLE_SLS && SE_DISABLE_DLS
    (void) sh;
#else
    while (abstractIfNeededLoop(sh))
        ;
#endif
}

namespace {

void spliceOutSegmentIfNeeded(SymHeap &sh, TObjId ao, TObjId peer,
                              TSymHeapList &todo)
{
    // check if the LS may be empty
    const TValueId addrSelf = sh.placedAt(ao);
    const TObjId nextPtrNext = subObjByChain(sh, peer, sh.objBinderField(peer));
    const TValueId valNext = sh.valueOf(nextPtrNext);
    bool eq;
    if (sh.proveEq(&eq, addrSelf, valNext)) {
        if (eq)
            // self loop?
            TRAP;

        // segment is _guaranteed_ to be non-empty now, but the concretization
        // makes it _possibly_ empty 
        sh.delNeq(addrSelf, valNext);
        return;
    }

    // possibly empty LS
    SymHeap sh0(sh);
    if (ao != peer) {
        // OK_DLS --> destroy peer
        const TFieldIdxChain icPrev = sh0.objBinderField(ao);
        const TValueId valPrev = sh0.valueOf(subObjByChain(sh0, ao, icPrev));
        sh0.valReplace(sh0.placedAt(peer), valPrev);
        sh0.objDestroy(peer);
    }

    // destroy self
    sh0.valReplace(addrSelf, valNext);
    sh0.objDestroy(ao);

    // schedule the empty variant for processing
    todo.push_back(sh0);
}

} // namespace

void concretizeObj(SymHeap &sh, TObjId obj, TSymHeapList &todo) {
    TObjId ao = obj;

    // branch by SLS/DLS
    const EObjKind kind = sh.objKind(obj);
    switch (kind) {
        case OK_CONCRETE:
            // invalid call of concretizeObj()
            TRAP;

        case OK_SLS:
            break;

        case OK_DLS:
            // jump to peer
            skipObj(sh, &ao, sh.objPeerField(obj));
            break;
    }

    // handle possibly empty variant (if exists)
    spliceOutSegmentIfNeeded(sh, obj, ao, todo);

    // duplicate self as abstract object
    const TObjId aoDup = sh.objDup(obj);
    const TValueId aoDupAddr = sh.placedAt(aoDup);
    if (OK_DLS == kind) {
        // DLS relink
        const TObjId peerField = subObjByChain(sh, ao, sh.objPeerField(ao));
        sh.objSetValue(peerField, aoDupAddr);
    }

    // concretize self and recover the list
    const TObjId ptrNext = subObjByChain(sh, obj, (OK_SLS == kind)
            ? sh.objBinderField(obj)
            : sh.objPeerField(obj));
    sh.objConcretize(obj);
    sh.objSetValue(ptrNext, aoDupAddr);

    if (OK_DLS == kind) {
        // update DLS back-link
        const TFieldIdxChain icPrev = sh.objBinderField(aoDup);
        const TObjId backLink = subObjByChain(sh, aoDup, icPrev);
        sh.objSetValue(backLink, sh.placedAt(obj));
    }
}
