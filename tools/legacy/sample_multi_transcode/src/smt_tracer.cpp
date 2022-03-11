/*############################################################################
  # Copyright (C) 2005 Intel Corporation
  #
  # SPDX-License-Identifier: MIT
  ############################################################################*/

#include "smt_tracer.h"

namespace TranscodingSample {

SMTTracer::SMTTracer() : Log(), AddonLog(), E2ELatency(), EncLatency(), TracerFileMutex() {
    TimeBase = std::chrono::steady_clock::now();
}

SMTTracer::~SMTTracer() {
    if (!Enabled)
        return;

    //these functions are intentionally called from destructor to try to save traces in case of a crash
    AddFlowEvents();
    SaveTrace(0xffffff & GetCurrentTS());

    ComputeE2ELatency();
    PrintE2ELatency();

    ComputeEncLatency();
    PrintEncLatency();
}

void SMTTracer::Init() {
    if (Enabled) {
        return;
    }
    Enabled = true;
    Log.reserve(TraceBufferSizeInMBytes * 1024 * 1024 / sizeof(Event));
}

void SMTTracer::BeginEvent(const ThreadType thType,
                           const mfxU32 thID,
                           const EventName name,
                           const void* inID,
                           const void* outID) {
    if (!Enabled)
        return;
    AddEvent(EventType::DurationStart, thType, thID, name, inID, outID);
}

void SMTTracer::EndEvent(const ThreadType thType,
                         const mfxU32 thID,
                         const EventName name,
                         const void* inID,
                         const void* outID) {
    if (!Enabled)
        return;
    AddEvent(EventType::DurationEnd, thType, thID, name, inID, outID);
}

void SMTTracer::AddCounterEvent(const ThreadType thType,
                                const mfxU32 thID,
                                const EventName name,
                                const mfxU64 counter) {
    if (!Enabled)
        return;
    AddEvent(EventType::Counter, thType, thID, name, reinterpret_cast<void*>(counter), nullptr);
}

void SMTTracer::SaveTrace(mfxU32 FileID) {
    std::string FileName = "smt_trace_" + std::to_string(FileID) + ".json";
    std::ofstream trace_file(FileName, std::ios::out);
    if (!trace_file) {
        return;
    }

    printf("\n### trace buffer usage [%d/%d] %.2f%%\n",
           (int)Log.size(),
           (int)Log.capacity(),
           100. * Log.size() / Log.capacity());
    printf("trace file name %s\n", FileName.c_str());

    trace_file << "[" << std::endl;

    for (const Event ev : Log) {
        WriteEvent(trace_file, ev);
    }
    for (const Event ev : AddonLog) {
        WriteEvent(trace_file, ev);
    }

    trace_file.close();
}

void SMTTracer::AddEvent(const EventType evType,
                         const ThreadType thType,
                         const mfxU32 thID,
                         const EventName name,
                         const void* inID,
                         const void* outID) {
    Event ev;
    ev.EvType = evType;
    ev.ThType = thType;
    ev.ThID   = thID;
    ev.Name   = name;
    ev.InID   = reinterpret_cast<mfxU64>(inID);
    ev.OutID  = reinterpret_cast<mfxU64>(outID);
    ev.TS     = GetCurrentTS();

    if (Log.size() == Log.capacity()) {
        return;
    }

    std::lock_guard<std::mutex> guard(TracerFileMutex);
    Log.push_back(ev);
}

mfxU64 SMTTracer::GetCurrentTS() {
    std::chrono::steady_clock::time_point time = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(time - TimeBase).count();
}

void SMTTracer::AddFlowEvents() {
    for (auto it = Log.begin(); it != Log.end(); ++it) {
        if (it->EvType != EventType::DurationStart) {
            continue;
        }

        if (it->InID == 0) {
            continue;
        }

        auto itc = std::reverse_iterator<decltype(it)>(it);

        auto itp = find_if(itc, Log.rend(), [&it](Event ev) {
            return ev.EvType == EventType::DurationEnd && it->InID == ev.OutID;
        });
        if (itp == Log.rend()) {
            continue;
        }

        AddFlowEvent(*itp, *it);
    }
}

void SMTTracer::ComputeE2ELatency() {
    NumOfErrors = 0;

    for (auto it = Log.begin(); it != Log.end(); ++it) {
        //look for end of sync operation in enc channel
        if (it->EvType != EventType::DurationEnd || it->ThType != ThreadType::ENC ||
            it->Name != EventName::SYNC) {
            continue;
        }

        EventIt bit = FindBeginningOfDependencyChain(it);
        if (bit == Log.end() || bit->ThType != ThreadType::DEC) {
            NumOfErrors++;
            continue;
        }

        E2ELatency[it->ThID].push_back(it->TS - bit->TS);
    }
}

void SMTTracer::PrintE2ELatency() {
    printf("\nEnd To End Latency (from decode or YUV read start to encode end)\n");
    printf("    number of frames with unknown latency: %d\n", NumOfErrors);

    for (const auto& v : E2ELatency) {
        printf("\n    enc%d number of frame %d\n", v.first, int(v.second.size()));
        printf("        per frame latency ms : ");
        for (mfxU64 t : v.second) {
            printf(" %.2f,", t / 1000.);
        }
        printf("\n");
    }
}

void SMTTracer::ComputeEncLatency() {
    NumOfErrors = 0;

    for (auto it = Log.begin(); it != Log.end(); ++it) {
        if (it->EvType != EventType::DurationEnd || it->ThType != ThreadType::ENC ||
            it->Name != EventName::SYNC) {
            continue;
        }

        EventIt syncEnd   = it;
        EventIt syncBegin = FindBeginningOfDurationEvent(syncEnd);
        EventIt encEnd    = FindEndOfPreviosDurationEvent(syncBegin);
        EventIt encBegin  = FindBeginningOfDurationEvent(encEnd);

        if (encBegin == Log.end() || encBegin->ThType != ThreadType::ENC) {
            NumOfErrors++;
            continue;
        }

        EncLatency[encBegin->ThID].push_back(syncEnd->TS - encBegin->TS);
    }
}

void SMTTracer::PrintEncLatency() {
    printf("\nEncode Latency (from encode start to encode end)\n");
    printf("    number of frames with unknown latency: %d\n", NumOfErrors);

    for (const auto& v : EncLatency) {
        printf("\n    enc%d number of frame %d\n", v.first, int(v.second.size()));
        printf("        per frame latency ms : ");
        for (mfxU64 t : v.second) {
            printf(" %.2f,", t / 1000.);
        }
        printf("\n");
    }
}

SMTTracer::EventIt SMTTracer::FindBeginningOfDependencyChain(EventIt it) {
    //"it" should point to the end of duration event
    if (it == Log.begin() || it == Log.end() || it->EvType != EventType::DurationEnd) {
        return Log.end();
    }

    for (;;) {
        it = FindBeginningOfDurationEvent(it);
        if (it == Log.end() || it->InID == 0)
            break;

        it = FindEndOfPreviosDurationEvent(it);
        if (it == Log.end())
            break;
    }

    return it;
}

SMTTracer::EventIt SMTTracer::FindBeginningOfDurationEvent(EventIt it) {
    //"it" should point to the end of duration event
    if (it == Log.begin() || it == Log.end() || it->EvType != EventType::DurationEnd) {
        return Log.end();
    }

    EventIt itc = it;
    for (; itc != Log.begin();) {
        --itc;
        if (itc->EvType == EventType::DurationStart && itc->ThType == it->ThType &&
            itc->ThID == it->ThID && itc->Name == it->Name) {
            return itc;
        }
    }

    return Log.end();
}

SMTTracer::EventIt SMTTracer::FindEndOfPreviosDurationEvent(EventIt it) {
    //"it" should point to the beginnig of duration event
    if (it == Log.begin() || it == Log.end() || it->EvType != EventType::DurationStart) {
        return Log.end();
    }

    EventIt itc = it;
    for (; itc != Log.begin();) {
        --itc;
        if (itc->EvType == EventType::DurationEnd && itc->OutID == it->InID) {
            return itc;
        }
    }
    return Log.end();
}

void SMTTracer::AddFlowEvent(const Event a, const Event b) {
    if (a.EvType != EventType::DurationEnd || b.EvType != EventType::DurationStart) {
        return;
    }

    Event ev;
    ev.EvType = EventType::FlowStart;
    ev.ThType = a.ThType;
    ev.ThID   = a.ThID;
    ev.EvID   = ++EvID;
    ev.TS     = a.TS;
    AddonLog.push_back(ev);

    ev.EvType = EventType::FlowEnd;
    ev.ThType = b.ThType;
    ev.ThID   = b.ThID;
    ev.EvID   = EvID;
    ev.TS     = b.TS;

    if (a.TS == b.TS) {
        ev.TS++;
    }

    AddonLog.push_back(ev);
}

void SMTTracer::WriteEvent(std::ofstream& trace_file, const Event ev) {
    switch (ev.EvType) {
        case EventType::DurationStart:
        case EventType::DurationEnd:
            WriteDurationEvent(trace_file, ev);
            break;
        case EventType::FlowStart:
        case EventType::FlowEnd:
            WriteFlowEvent(trace_file, ev);
            break;
        case EventType::Counter:
            WriteCounterEvent(trace_file, ev);
            break;
        default:;
    }
}

void SMTTracer::WriteDurationEvent(std::ofstream& trace_file, const Event ev) {
    trace_file << "{";
    WriteEventPID(trace_file);
    WriteComma(trace_file);
    WriteEventTID(trace_file, ev);
    WriteComma(trace_file);
    WriteEventTS(trace_file, ev);
    WriteComma(trace_file);
    WriteEventPhase(trace_file, ev);
    WriteComma(trace_file);
    WriteEventName(trace_file, ev);
    WriteComma(trace_file);
    WriteEventInOutIDs(trace_file, ev);
    trace_file << "}," << std::endl;
}

void SMTTracer::WriteFlowEvent(std::ofstream& trace_file, const Event ev) {
    trace_file << "{";
    WriteEventPID(trace_file);
    WriteComma(trace_file);
    WriteEventTID(trace_file, ev);
    WriteComma(trace_file);
    WriteEventTS(trace_file, ev);
    WriteComma(trace_file);
    WriteEventPhase(trace_file, ev);
    WriteComma(trace_file);
    WriteEventName(trace_file, ev);
    WriteComma(trace_file);
    WriteBindingPoint(trace_file, ev);
    WriteComma(trace_file);
    WriteEventCategory(trace_file);
    WriteComma(trace_file);
    WriteEvID(trace_file, ev);
    trace_file << "}," << std::endl;
}

void SMTTracer::WriteCounterEvent(std::ofstream& trace_file, const Event ev) {
    trace_file << "{";
    WriteEventPID(trace_file);
    WriteComma(trace_file);
    WriteEventTID(trace_file, ev);
    WriteComma(trace_file);
    WriteEventTS(trace_file, ev);
    WriteComma(trace_file);
    WriteEventPhase(trace_file, ev);
    WriteComma(trace_file);
    WriteEventName(trace_file, ev);
    WriteComma(trace_file);
    WriteEventCounter(trace_file, ev);
    trace_file << "}," << std::endl;
}

void SMTTracer::WriteEventPID(std::ofstream& trace_file) {
    trace_file << "\"pid\":\"smt\"";
}

void SMTTracer::WriteEventTID(std::ofstream& trace_file, const Event ev) {
    trace_file << "\"tid\":\"";
    switch (ev.ThType) {
        case ThreadType::DEC:
            trace_file << "dec";
            break;
        case ThreadType::VPP:
            trace_file << "enc" << ev.ThID;
            break;
        case ThreadType::ENC:
            trace_file << "enc" << ev.ThID;
            break;
        case ThreadType::CSVPP:
            trace_file << "vpp" << ev.ThID;
            break;
        default:
            trace_file << "unknown";
            break;
    }
    trace_file << "\"";
}

void SMTTracer::WriteEventTS(std::ofstream& trace_file, const Event ev) {
    trace_file << "\"ts\":" << ev.TS;
}

void SMTTracer::WriteEventPhase(std::ofstream& trace_file, const Event ev) {
    trace_file << "\"ph\":\"";

    switch (ev.EvType) {
        case EventType::DurationStart:
            trace_file << "B";
            break;
        case EventType::DurationEnd:
            trace_file << "E";
            break;
        case EventType::FlowStart:
            trace_file << "s";
            break;
        case EventType::FlowEnd:
            trace_file << "f";
            break;
        case EventType::Counter:
            trace_file << "C";
            break;
        default:
            trace_file << "unknown";
            break;
    }
    trace_file << "\"";
}

void SMTTracer::WriteEventName(std::ofstream& trace_file, const Event ev) {
    trace_file << "\"name\":\"";
    if (ev.EvType == EventType::FlowStart || ev.EvType == EventType::FlowEnd) {
        trace_file << "link";
    }
    else if (ev.EvType == EventType::Counter) {
        switch (ev.ThType) {
            case ThreadType::DEC:
                trace_file << "dec_pool";
                break;
            case ThreadType::VPP:
                trace_file << "enc_pool" << ev.ThID;
                break;
            case ThreadType::ENC:
                trace_file << "enc_pool" << ev.ThID;
                break;
            case ThreadType::CSVPP:
                trace_file << "vpp_pool" << ev.ThID;
                break;
            default:
                trace_file << "unknown";
                break;
        }
    }
    else if (ev.Name != EventName::UNDEF) {
        switch (ev.Name) {
            case EventName::BUSY:
                trace_file << "busy";
                break;
            case EventName::SYNC:
                trace_file << "syncp";
                break;
            case EventName::READ_YUV:
            case EventName::READ_BS:
                trace_file << "read";
                break;
            case EventName::SURF_WAIT:
                trace_file << "wait";
                break;
            default:
                trace_file << "unknown";
                break;
        }
    }
    else {
        switch (ev.ThType) {
            case ThreadType::DEC:
                trace_file << "dec";
                break;
            case ThreadType::VPP:
                trace_file << "vpp";
                break;
            case ThreadType::ENC:
                trace_file << "enc";
                break;
            case ThreadType::CSVPP:
                trace_file << "csvpp";
                break;
            default:
                trace_file << "unknown";
                break;
        }
    }
    trace_file << "\"";
}

void SMTTracer::WriteBindingPoint(std::ofstream& trace_file, const Event ev) {
    if (ev.EvType != EventType::FlowStart && ev.EvType != EventType::FlowEnd) {
        return;
    }
    trace_file << "\"bp\":\"e\"";
}

void SMTTracer::WriteEventInOutIDs(std::ofstream& trace_file, const Event ev) {
    trace_file << "\"args\":{\"InID\":" << ev.InID << ",\"OutID\":" << ev.OutID << "}";
}

void SMTTracer::WriteEventCounter(std::ofstream& trace_file, const Event ev) {
    trace_file << "\"args\":{\"free surfaces\":" << ev.InID << "}";
}

void SMTTracer::WriteEventCategory(std::ofstream& trace_file) {
    trace_file << "\"cat\":\"link\"";
}

void SMTTracer::WriteEvID(std::ofstream& trace_file, const Event ev) {
    trace_file << "\"id\":\"id_" << ev.EvID << "\"";
}

void SMTTracer::WriteComma(std::ofstream& trace_file) {
    trace_file << ",";
}

} // namespace TranscodingSample
