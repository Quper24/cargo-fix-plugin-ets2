"""Read-only schema evidence for Road Trip/Coaches. Never loads DLLs or edits saves.

Names below describe field-set roles, not recovered native class symbols.
Metadata presence does not prove DLC availability or safe constructor ABI.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
from compare_executables import Image, ROOT

SNAPSHOTS = {
    "ets2-public-1.62.3.3": "eurotrucks2.exe",
    "ets2-public-1.62.1.0": "eurotrucks2.exe",
    "ats-public-1.62.2.2": "amtrucks.exe",
    "ats-public-1.62.1.1": "amtrucks.exe",
    "ats-oculus-1.62.1.1001": "amtrucks.exe",
    "ats-oculus-1.62.1.1001-build-26457597-staged": "amtrucks.exe",
    "public_beta-1.62.0.10": "eurotrucks2.exe",
}
ROLES = {
    "economy": {"bank", "player", "companies", "bus_stops", "car_job_generator"},
    "player": {"buses", "cars", "current_bus_job", "current_car_job", "selected_bus_offer"},
    "coach_stop_offers": {"discovered", "lines_offer"},
    "coach_offer": {"source", "saved_targets", "targets_distances", "targets_times", "targets_ferry_times", "targets_base_times", "departure_time_of_day", "bus_name", "expiration_time"},
    "coach_active_job": {"stages", "stage_idx", "passengers", "left_passengers"},
    "coach_stage": {"previous_stage", "source_bus_stop", "target_bus_stop", "planned_departure_time", "planned_arrival_time", "planned_entering_passenger_count", "planned_leaving_passenger_count"},
    "car_job_definition": {"cargos", "vehicle_types", "source_data_contexts", "target_data_contexts", "required_courier_rating"},
    "car_offer": {"job_type", "shortest_distance_km", "company_car", "expiration_time", "units_count", "job", "cargo"},
    "car_active_job": {"company_car", "source_company", "target_company", "start_time", "units_count"},
}

def schemas(image):
    data = image.data
    u64 = lambda at: struct.unpack_from("<Q", data, at)[0]
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sections = []
    for n in range(count):
        at = pe + 24 + opt_size + n * 40
        size, rva = struct.unpack_from("<II", data, at + 8)
        flags = struct.unpack_from("<I", data, at + 36)[0]
        sections.append((rva, size, flags))
    def name(pointer):
        at = pointer - image.base
        if not 0 <= at < len(data):
            return ""
        raw = bytes(data[at:at+128]).split(b"\0")[0]
        return raw.decode("ascii") if re.fullmatch(rb"[a-z][a-z0-9_]*", raw) else ""
    def owner_valid(pointer):
        at = pointer - image.base
        return any(rva <= at < rva + size and flags & 0x40000000 and not flags & 0xa0000000 for rva,size,flags in sections)
    groups = []
    for rva, size, flags in sections:
        if flags & 0xc0000000 != 0xc0000000 or flags & 0x20000000:
            continue
        for at in range(rva + 40, rva + size - 40, 8):
            owner = u64(at)
            if u64(at-40) == owner or u64(at+8) > 0x4000 or u64(at+16) > 0x100 or not owner_valid(owner):
                continue
            if not name(u64(at+32)):
                continue
            fields = []
            for n in range(400):
                row = at + n * 40
                if row + 40 > len(data) or u64(row) != owner:
                    break
                label = name(u64(row+32))
                if not label:
                    break
                fields.append({"name":label,"offset":u64(row+8),"type":u64(row+16)})
            if fields:
                groups.append({"metadata_rva":at,"fields":fields})
    return groups

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT/"build-dev-research/multimode/report.json")
    args = parser.parse_args()
    report = {"scope":"Static metadata only; no runtime mutation or constructor validation", "snapshots":{}}
    for snapshot, exe in SNAPSHOTS.items():
        path = ROOT/"diagnostics/executables"/snapshot/exe
        image = Image(path)
        groups = schemas(image)
        roles = {role:[g for g in groups if required <= {f["name"] for f in g["fields"]}] for role,required in ROLES.items()}
        report["snapshots"][snapshot] = {"sha256_evidence_only":hashlib.sha256(path.read_bytes()).hexdigest(),"roles":roles}
        print(snapshot + ": " + ", ".join(f"{role}={len(candidates)}" for role,candidates in roles.items()))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(args.output)

if __name__ == "__main__":
    main()
