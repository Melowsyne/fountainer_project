import sys,time,struct,json,base64; sys.path.insert(0,"/tmp")
from scopelib import Scope
s=Scope(); s.clear()
def w(c): s.write(c)
def wave(ch):
    for i in range(4):
        try:
            pre,data=s.waveform(ch,100000); f=pre.split(";")
            if len(data)>1000 and max(struct.unpack(f"{len(data)}b",data))>-128:
                return {"ymult":float(f[13]),"yoff":float(f[14]),"yzero":float(f[15]),"xincr":float(f[9]),"xzero":float(f[10]),"raw":base64.b64encode(data).decode()}
        except Exception as e: print("read err",e)
        s.clear(); time.sleep(0.5)
    return None
def arm(tmo=12):
    w("ACQuire:STATE STOP"); w("TRIGger:A:EDGE:SOUrce CH2"); w("TRIGger:A:EDGE:SLOpe RISE"); w("TRIGger:A:LEVel 3.0")
    w("TRIGger:A:MODe NORMal"); w("ACQuire:STOPAfter SEQuence"); w("ACQuire:STATE RUN")
    t=time.time()
    while time.time()-t<tmo:
        if s.safe_ask("TRIGger:STATE?").startswith("SAV"): time.sleep(0.5); return True
        time.sleep(0.15)
    return False
w("SELect:CH1 ON"); w("SELect:CH2 ON"); w("SELect:MATH OFF")
for ch in ("CH1","CH2"): w(f"{ch}:SCAle 1"); w(f"{ch}:COUPling DC"); w(f"{ch}:BANdwidth FULl"); w(f"{ch}:POSition -2")
w("ACQuire:MODe SAMple"); w("HORizontal:DELay:MODe ON")
mode=sys.argv[1]; n=int(sys.argv[2]); tdiv=sys.argv[3]; delay=sys.argv[4]
w(f"HORizontal:SCAle {tdiv}"); w(f"HORizontal:DELay:TIMe {delay}"); print("delay set:", s.safe_ask("HORizontal:DELay:TIMe?"), "pos:", s.safe_ask("HORizontal:POSition?"))
caps=[]
for k in range(n):
    if not arm(): print("no trigger"); continue
    c1=wave("CH1"); c2=wave("CH2")
    if c1 and c2: caps.append({"ch1":c1,"ch2":c2,"tdiv":tdiv,"t":time.time()}); print("captured",k)
    time.sleep(0.3)
json.dump(caps,open(f"/tmp/caps_{mode}.json","w"))
print(mode,len(caps),"captures")
w("ACQuire:STATE STOP"); w("TRIGger:A:MODe AUTO"); w("ACQuire:STOPAfter RUNSTop"); w("ACQuire:STATE RUN")
