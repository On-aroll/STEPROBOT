"""Test-local extensions for actual batch ST, including raw nonblocking socket calls.

The historical interpreter is left untouched. This is not an IEC compiler.
"""
import st_runtime as rt

OldInstance, OldWorld, OldPointer = rt.Instance, rt.World, rt.Pointer

class Pointer(OldPointer):
    def __init__(self, env, name, offset=0):
        super().__init__(env,name); self.offset=offset
    def __add__(self,n): return Pointer(self.env,self.name,self.offset+n)
    def __getitem__(self,n): return super().__getitem__(self.offset+n)
    def __setitem__(self,n,v): super().__setitem__(self.offset+n,v)

class Struct:
    def __init__(self,**values): self.values=values
    def get(self,name,raw=False):
        root,sep,tail=name.partition('.')
        return self.values[root].get(tail,raw) if sep else self.values[name]
    def set(self,name,value): self.values[name]=value

class Instance(OldInstance):
    def __init__(self,*args,**kwargs):
        super().__init__(*args,**kwargs)
        for name,kind in self.kinds.items():
            if kind=='CmpHCTCPIP.SOCKET_INFO':
                self.values[name]=Struct(hsocket=-1,disockettype=0)
    def evaluate(self,node):
        if node[0]=='function':
            name=node[1].lower()
            if name=='adr':
                ref=node[2][0]
                if ref[0]!='var': raise SyntaxError('ADR requires a variable')
                value=self.get(ref[1])
                if ref[2] is not None:
                    index=self.evaluate(ref[2])
                    return value+index if isinstance(value,OldPointer) else Pointer(self,ref[1],index)
                return Pointer(self,ref[1])
            if name.endswith('_to_dint'): return int(self.evaluate(node[2][0]))
            if name in ('cmphctcpip.getsocketinfo','syssocket.syssocksend','syssocket.syssockrecv'):
                return self.world.raw(name,[self.evaluate(arg) for arg in node[2]])
        return super().evaluate(node)

class World(OldWorld):
    def __init__(self,*args,**kwargs):
        super().__init__(*args,**kwargs)
        self.globals['syssocket']=Struct(rts_invalid_handle=-1,socket_stream=1,socket_msg_dontwait=64)
        # CmpErrors 3.3.1.40 uses RTS result codes, not native Winsock codes.
        self.globals['cmperrors']=Struct(errors=Struct(err_ok=0,err_sock_wouldblock=0x0206,err_sock_closed=0x0211))
        self.calls={};self.send_calls=0;self.recv_calls=0
    def raw(self,name,args):
        server=self.native_factory.__self__
        if name=='cmphctcpip.getsocketinfo':
            cid,p=args
            if cid not in server.connections:return 1
            info=p.env.get(p.name)
            info.set('hsocket',cid);info.set('disockettype',1)
            return 0
        cid,p,count,flags,result=args
        assert flags==64 and 0<count<=8192
        result.env.set(result.name,0)
        key=(name,cid);self.calls[key]=self.calls.get(key,0)+1
        call=self.calls[key]
        block=getattr(server,'wouldblock_every',0)
        if block and call%block==0:
            result.env.set(result.name,0x0206);return -1
        if getattr(server,'raw_error',False):result.env.set(result.name,123);return -1
        assert cid in server.connections
        if name=='syssocket.syssocksend':
            self.send_calls+=1
            sizes=getattr(server,'send_sizes',(8192,))
            count=min(count,sizes[(call-1)%len(sizes)])
            data=bytes(p[i] for i in range(count))
            if cid==245:
                server.command.extend(data)
                while b'\r\n' in server.command:
                    line,_,tail=server.command.partition(b'\r\n')
                    server.command=bytearray(tail);server.line(line.decode())
            else:server.upload.extend(data)
            return count
        self.recv_calls+=1
        queue=server.control if cid==245 else server.download
        if queue:
            sizes=getattr(server,'recv_sizes',(8192,))
            count=min(count,len(queue),sizes[(call-1)%len(sizes)])
            for i in range(count):p[i]=queue[i]
            del queue[:count]
            return count
        if cid==246 and server.mode=='retr':
            result.env.set(result.name,getattr(server,'recv_end_result',0))
            return getattr(server,'recv_end_count',0)
        result.env.set(result.name,0x0206);return -1

def install(legacy):
    rt.Instance=Instance;rt.World=World;rt.Pointer=Pointer
    legacy.World=World;legacy.Pointer=Pointer
