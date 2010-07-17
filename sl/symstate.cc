/*
 * Copyright (C) 2009-2010 Kamil Dudka <kdudka@redhat.com>
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
#include "symstate.hh"

#include <cl/cl_msg.hh>
#include <cl/code_listener.h>

#include "util.hh"
#include "worklist.hh"

#include <iomanip>
#include <map>
#include <stack>

#include <boost/foreach.hpp>
#include <boost/tuple/tuple.hpp>

// /////////////////////////////////////////////////////////////////////////////
// SymHeapUnion implementation
namespace {
    bool checkNonPosValues(int a, int b) {
        if (0 < a && 0 < b)
            // we'll need to properly compare positive values
            return false;

        // non-positive values always have to match, bail out otherwise
        return a != b;
    }

    template <class TSubst>
    bool matchValues(TSubst &subst, TValueId v1, TValueId v2) {
        if (checkNonPosValues(v1, v2))
            // null vs. non-null, etc.
            return false;

        // we need to have the values always in the same order to guarantee
        // the substitution to be bijective ... there used to be a nasty bug
        // at this point, leading to the following nonsense:
        //   [17] = 17
        //   [18] = 18
        //   [35] = 17
        //   [36] = 18
        sortValues(v1, v2);

        typename TSubst::iterator iter = subst.find(v1);
        if (iter != subst.end())
            // substitution already defined, check if it applies seamlessly
            return iter->second == v2;

        // define a new substitution
        subst[v1] = v2;
        return true;
    }

    template <class TSubst, class THeap>
    bool matchValues(TSubst &subst, const THeap &heap1, const THeap &heap2,
                     TValueId v1, TValueId v2)
    {
        if (v1 <= 0 || v2 <= 0)
            // this can't be a pair of custom or unknown values
            return matchValues(subst, v1, v2);

        // do we know the values?
        const EUnknownValue uv1 = heap1.valGetUnknown(v1);
        const EUnknownValue uv2 = heap2.valGetUnknown(v2);
        if (uv1 != uv2)
            // mismatch in kind of unknown values
            return false;

        const int cVal1 = heap1.valGetCustom(0, v1);
        const int cVal2 = heap2.valGetCustom(0, v2);
        if (OBJ_INVALID == cVal1 && OBJ_INVALID == cVal2)
            // this can't be a pair of custom values
            return matchValues(subst, v1, v2);

        if (OBJ_INVALID == cVal1 || OBJ_INVALID == cVal2)
            // custom and non-custom values are going to be compared
            TRAP;

        // match custom values
        return (cVal1 == cVal2);
    }

    template <class THeap>
    bool skipValue(const THeap &heap, TValueId value) {
        if (OBJ_INVALID != heap.valGetCompositeObj(value))
            // compare composite objects recursively
            return false;

        if (value <= 0)
            // no need for next wheel (special values already handled)
            return true;

        if (-1 != heap.valGetCustom(0, value))
            // don't follow fnc pointers (and other custom values) by pointsTo()
            return true;

        const EUnknownValue code = heap.valGetUnknown(value);
        switch (code) {
            case UV_KNOWN:
            case UV_ABSTRACT:
                return false;

            default:
                // don't follow uknown values
                return true;
        }
    }

    template<class THeap>
    bool isComposite(const THeap &heap1, const THeap &heap2,
                     TValueId value1, TValueId value2)
    {
        const TObjId cObj1 = heap1.valGetCompositeObj(value1);
        const TObjId cObj2 = heap2.valGetCompositeObj(value2);
        if (OBJ_INVALID == cObj1 && OBJ_INVALID == cObj2)
            return false;

        if (OBJ_INVALID == cObj1 || OBJ_INVALID == cObj2)
            // type mismatch (scalar vs. composite ought to be compared)
            TRAP;

        return true;
    }

    template<class TWL, class THeap>
    bool digComposite(TWL &wl, const THeap &heap1, const THeap &heap2,
                      TValueId value1, TValueId value2)
    {
        const TObjId cObj1 = heap1.valGetCompositeObj(value1);
        const TObjId cObj2 = heap2.valGetCompositeObj(value2);
        // cObj1 and cObj2 are supposed to be valid at this point, see
        // isComposite()

        typedef std::pair<TObjId, TObjId> TItem;
        std::stack<TItem> todo;
        push(todo, cObj1, cObj2);
        while (!todo.empty()) {
            TObjId o1, o2;
            boost::tie(o1, o2) = todo.top();
            todo.pop();

            const struct cl_type *clt = heap1.objType(o1);
            if (clt != heap2.objType(o2))
                // type mismatch
                return false;

            const enum cl_type_e code = (clt)
                ? clt->code
                : /* anonymous object of known size */ CL_TYPE_PTR;     // segments?

            switch (code) {
                case CL_TYPE_PTR: {
                    const TValueId val1 = heap1.valueOf(o1);
                    const TValueId val2 = heap2.valueOf(o2);
                    wl.schedule(val1, val2);
                    break;
                }

                case CL_TYPE_STRUCT:
                    for (int i = 0; i < clt->item_cnt; ++i) {
                        const TObjId sub1 = heap1.subObj(o1, i);
                        const TObjId sub2 = heap2.subObj(o2, i);
                        if (sub1 < 0 || sub2 < 0)
                            TRAP;

                        push(todo, sub1, sub2);
                    }
                    break;

                case CL_TYPE_INT:
                    break;

                default:
                    // other type of values should be safe to ignore here
                    // but worth to check by a debugger at least once anyway
                    TRAP;
            }
        }
        return true;
    }

bool cmpAbstractObjects(const SymHeap &sh1, const SymHeap &sh2,
                        TObjId o1, TObjId o2)
{
    const EObjKind kind = sh1.objKind(o1);
    if (sh2.objKind(o2) != kind)
        // kind of object mismatch
        return false;

    if (OK_CONCRETE == kind)
        // no abstract objects involved
        return true;

    // compare 'next' and 'peer' pointers
    return sh1.objNextField(o1) == sh2.objNextField(o2)
        && sh1.objPeerField(o1) == sh2.objPeerField(o2);
}

} // namespace 

template <class TWL, class TSubst>
bool matchPreds(TWL             &wl,
                TSubst          &valSubst,
                const SymHeap   &heap1,
                const SymHeap   &heap2,
                TValueId        v1,
                TValueId        v2)
{
    SymHeap::TContValue rel1, rel2;
    heap1.gatherRelatedValues(rel1, v1);
    heap2.gatherRelatedValues(rel2, v2);

    const unsigned cnt = rel1.size();
    if (rel2.size() != cnt)
        return false;

    // We will probably need to extend the interface of SymHeap in order
    // to compare predicates enough efficiently...
#if 0
    // TODO
    if (cnt)
        TRAP;
#else
    (void) wl;
    (void) valSubst;
#endif

    return true;
}

template <class TWL, class TSubst>
bool dfsCmp(TWL             &wl,
            TSubst          &valSubst,
            const SymHeap   &heap1,
            const SymHeap   &heap2)
{
    // DFS loop
    typename TWL::value_type item;
    while (wl.next(item)) {
        TValueId value1, value2;
        boost::tie(value1, value2) = item;

        if (!matchPreds(wl, valSubst, heap1, heap2, value1, value2))
            return false;

        if (!matchValues(valSubst, heap1, heap2, value1, value2))
            // value mismatch
            return false;

        if (skipValue(heap1, value1))
            // no need for next wheel
            continue;

        if (isComposite(heap1, heap2, value1, value2)) {
            if (!digComposite(wl, heap1, heap2, value1, value2))
                // object type mismatch (something nasty in the analyzed code)
                return false;

            // compare composite objects recursively
            continue;
        }

        const TObjId obj1 = heap1.pointsTo(value1);
        const TObjId obj2 = heap2.pointsTo(value2);
        if (checkNonPosValues(obj1, obj2))
            // variable mismatch
            return false;

        if (!cmpAbstractObjects(heap1, heap2, obj1, obj2))
            // abstract objects are not equeal
            return false;

        value1 = heap1.valueOf(obj1);
        value2 = heap2.valueOf(obj2);

        // schedule values for next wheel
        wl.schedule(value1, value2);
    }

    // heaps are equal (isomorphism)
    return true;
}

bool operator== (const SymHeap &heap1, const SymHeap &heap2) {
    // DFS stack
    typedef std::pair<TValueId, TValueId> TValuePair;
    WorkList<TValuePair> wl;

    // value substitution (isomorphism)
    typedef std::map<TValueId, TValueId> TSubst;
    TSubst valSubst;

    // FIXME: suboptimal interface of SymHeap::gatherCVars()
    SymHeap::TContCVar cVars1, cVars2;
    heap1.gatherCVars(cVars1);
    heap1.gatherCVars(cVars2);
    if (cVars1.size() != cVars2.size())
        // different count of program variables
        // --> no chance the heaps are equal up to isomorphism
        return false;

    // FIXME: rewrite the following nonsense
    std::set<CVar> cVars;
    BOOST_FOREACH(CVar cv, cVars1) { cVars.insert(cv); }
    BOOST_FOREACH(CVar cv, cVars2) { cVars.insert(cv); }

    BOOST_FOREACH(CVar cv, cVars) {
        const TObjId var1 = heap1.objByCVar(cv);
        const TObjId var2 = heap2.objByCVar(cv);
        if (var1 < 0 || var2 < 0)
            // static variable mismatch
            return false;

        // retrieve values of static variables
        const TValueId value1 = heap1.valueOf(var1);
        const TValueId value2 = heap2.valueOf(var2);

        // schedule for DFS
        wl.schedule(value1, value2);
    }

    // run DFS
    return dfsCmp(wl, valSubst, heap1, heap2);
}

int SymHeapUnion::lookup(const SymHeap &heap) const {
    const int cnt = this->size();
    for(int idx = 0; idx < cnt; ++idx) {
        if (heap == heaps_[idx])
            return idx;
    }

    // not found
    return -1;
}

void SymHeapUnion::insert(const SymHeap &heap) {
    BOOST_FOREACH(const SymHeap &current, heaps_) {
        // TODO: check for entailment instead
        if (heap == current)
            return;
    }

    // add given heap to union
    heaps_.push_back(heap);
}

void SymHeapUnion::insert(const SymHeapUnion &huni) {
    BOOST_FOREACH(const SymHeap &current, huni) {
        this->insert(current);
    }
}

// vim: tw=80
