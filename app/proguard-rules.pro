# Native bridge dipanggil via JNI — jangan di-obfuscate namanya.
-keep class com.xykal.sampdroid.NativeBridge { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
