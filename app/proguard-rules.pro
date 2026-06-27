-keepclassmembers class com.throttlevpn.engine.Tun2SocksEngine {
    native <methods>;
    boolean protectSocket(int);
}

-keep class com.throttlevpn.engine.Tun2SocksEngine { *; }

-keepclassmembers class * extends android.net.VpnService {
    public *;
}

-keep class com.throttlevpn.receiver.BootReceiver { *; }
