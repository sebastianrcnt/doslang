# Place this directory at MTCP\APPS\TCPAGENT in an mTCP checkout.
tcp_c_dir = ..\..\TCPLIB
memory_model = -mc
compile_options = @WPP.RSP
tcpobjs = packet.obj arp.obj eth.obj ip.obj tcp.obj tcpsockm.obj udp.obj utils.obj timer.obj ipasm.obj trace.obj
all : tcpagent.exe
.cpp.obj :
  wpp $[* $(compile_options)
tcpagent.obj: tcpagent.cpp
  wpp $(compile_options) tcpagent.cpp
packet.obj:
  wpp $(tcp_c_dir)\packet.cpp $(compile_options)
arp.obj:
  wpp $(tcp_c_dir)\arp.cpp $(compile_options)
eth.obj:
  wpp $(tcp_c_dir)\eth.cpp $(compile_options)
ip.obj:
  wpp $(tcp_c_dir)\ip.cpp $(compile_options)
tcp.obj:
  wpp $(tcp_c_dir)\tcp.cpp $(compile_options)
tcpsockm.obj:
  wpp $(tcp_c_dir)\tcpsockm.cpp $(compile_options)
udp.obj:
  wpp $(tcp_c_dir)\udp.cpp $(compile_options)
utils.obj:
  wpp $(tcp_c_dir)\utils.cpp $(compile_options)
timer.obj:
  wpp $(tcp_c_dir)\timer.cpp $(compile_options)
trace.obj:
  wpp $(tcp_c_dir)\trace.cpp $(compile_options)
ipasm.obj:
  wasm -0 -mc $(tcp_c_dir)\ipasm.asm
tcpagent.exe: tcpagent.obj $(tcpobjs)
  wlink system dos option map option eliminate option stack=16384 name $@ file *.obj
clean : .symbolic
  del *.obj
  del *.map
  del tcpagent.exe
