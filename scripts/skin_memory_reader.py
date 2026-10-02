"""Read-only analysis helper for the verified September 30 build. No writes/injection."""
import ctypes as c, struct
class Memory:
    def __init__(self,pid):
        self.k=c.WinDLL('kernel32',use_last_error=True)
        self.k.OpenProcess.restype=c.c_void_p
        self.k.ReadProcessMemory.argtypes=[c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
        self.k.CloseHandle.argtypes=[c.c_void_p]
        self.h=self.k.OpenProcess(0x410,False,pid)
        if not self.h:raise OSError(c.get_last_error(),'OpenProcess')
    def read(self,a,n):
        if not 0<n<=32*1024*1024:raise ValueError('read bound')
        b=c.create_string_buffer(n);s=c.c_size_t()
        if not self.k.ReadProcessMemory(self.h,a,b,n,c.byref(s)) or s.value!=n:raise OSError(c.get_last_error(),hex(a))
        return b.raw
    def q(self,a):return struct.unpack('<Q',self.read(a,8))[0]
    def u(self,a):return struct.unpack('<I',self.read(a,4))[0]
    def close(self):self.k.CloseHandle(self.h)
if __name__=='__main__':
    import sys,capstone
    m=Memory(int(sys.argv[1]))
    try:
        for i in capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64).disasm(m.read(0x149031e60,128),0x149031e60):print(hex(i.address),i.mnemonic,i.op_str)
        for table in [0x14f2975e0,0x14f297a20]:
            p=m.q(table+96);count=m.u(table+104)
            print('TABLE',hex(table),count)
            for i in range(3):
                row=m.q(p+16*i);print(hex(row),m.read(row,144).hex() if row else '')
    finally:m.close()
