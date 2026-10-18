#!/system/bin/sh
# Allow su for uid 2000 (adb shell): policy 2 = allow, until 0 = forever.
for i in 1 2 3 4 5 6 7 8 9 10; do
	magisk --sqlite "REPLACE INTO policies (uid,policy,until,logging,notification) VALUES(2000,2,0,1,0)" && break
	sleep 3
done
