# DisplayXR Unity provider (displayxr-unity#418): R8 / ProGuard keep rules for the
# Java half of the Android provider, com.displayxr.unity.DisplayXRSurface.
#
# libdisplayxr_unity.so reaches that class ONLY through JNI: it loads it by name,
# calls attach(Activity) and binds the native* methods with RegisterNatives. Nothing
# in Java references it, so with Player Settings > Publishing Settings > Minify
# (Release / Debug) on, R8 removes it — measured: the class lands in R8's usage.txt
# (removed code) — and the provider silently falls back to a runtime-hosted
# surface with no touch submission.
#
# Applied by Editor/DisplayXRAndroidProguardProcessor.cs, which appends this file to
# the generated unityLibrary/proguard-unity.txt (that module's consumerProguardFiles,
# so it reaches the launcher's R8). Unity generates this Android Library's
# build.gradle itself, without consumerProguardFiles, and R8 does not read
# META-INF/proguard rules from a Gradle subproject — both were checked.
-keep class com.displayxr.unity.DisplayXRSurface { *; }
-keep class com.displayxr.unity.DisplayXRSurface$* { *; }
