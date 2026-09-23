import sys,time,subprocess; sys.path.insert(0,"/tmp")
from scopelib import Scope
s=Scope(); s.clear()
def w(c): s.write(c)
def arm(src,slope,lvl,tmo=12):
    w("ACQuire:STATE STOP"); w(f"TRIGger:A:EDGE:SOUrce {src}"); w(f"TRIGger:A:EDGE:SLOpe {slope}"); w(f"TRIGger:A:LEVel {lvl}")
    w("TRIGger:A:MODe NORMal"); w("ACQuire:STOPAfter SEQuence"); w("ACQuire:STATE RUN")
    t=time.time()
    while time.time()-t<tmo:
        if s.safe_ask("TRIGger:STATE?").startswith("SAV"): time.sleep(0.5); return True
        time.sleep(0.2)
    return False
def shot(name):
    n=s.screenshot(f"/tmp/{name}.png"); print(name, n, "bytes")
# common setup
w("HARDCopy:INKSaver OFF"); w("DISplay:PERSistence OFF")
w("SELect:CH1 ON"); w("SELect:CH2 ON"); w("SELect:MATH OFF")
w('CH1:LABel "CAN_L"'); w('CH2:LABel "CAN_H"')
for ch in ("CH1","CH2"): w(f"{ch}:SCAle 1"); w(f"{ch}:COUPling DC"); w(f"{ch}:BANdwidth TWEnty")
w("CH2:POSition -0.5"); w("CH1:POSition -3.5")      # CAN_H upper, CAN_L lower trace
w("HORizontal:POSition 15")
mode=sys.argv[1]
if mode=="idle":   # heartbeat only
    w("HORizontal:SCAle 40E-6");  print("A:", arm("CH2","RISE",3.0)); shot("can_01_heartbeat_frame_40us")
    w("HORizontal:SCAle 4E-6");   print("B:", arm("CH2","RISE",3.0)); shot("can_02_bit_level_4us")
    w("MATH:DEFine \"CH2-CH1\""); w("SELect:MATH ON"); w("MATH:VERTical:SCAle 1"); w("MATH:VERTical:POSition -2")
    w("CH1:POSition -4"); w("CH2:POSition 1.5")
    w("HORizontal:SCAle 40E-6");  print("C:", arm("CH2","RISE",3.0)); shot("can_03_differential_math_40us")
    w("SELect:MATH OFF"); w("CH2:POSition -0.5"); w("CH1:POSition -3.5")
else:              # under SDO load (bench running)
    w("HORizontal:SCAle 200E-6"); print("D:", arm("CH2","RISE",3.0)); shot("can_04_sdo_request_response_200us")
    w("HORizontal:SCAle 2E-3");   print("E:", arm("CH2","RISE",3.0)); shot("can_05_bus_traffic_2ms")
    w("HORizontal:SCAle 10E-6");  print("F:", arm("CH2","RISE",3.0)); shot("can_06_edges_10us")
w("ACQuire:STATE STOP"); w("TRIGger:A:MODe AUTO"); w("ACQuire:STOPAfter RUNSTop"); w("ACQuire:STATE RUN")
