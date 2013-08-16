package com.ivygroup.wfdplayer;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

public class MyBroadcastReceiver extends BroadcastReceiver {
    private static final String TAG = "MyBroadcastReceiver";

    @Override
    public void onReceive(Context context, Intent intent) {
        Log.d(TAG, "receive broadcast ");

        String host = intent.getStringExtra("host");
        int port = intent.getIntExtra("port", 7236);
        Log.d(TAG, "Receive the link broadcast. host = " + host + ", port = " + port);

        Intent targetIntent = new Intent(context, MainActivity.class);
        targetIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        targetIntent.putExtra("host", host);
        targetIntent.putExtra("port", port);

        context.startActivity(targetIntent);
    }

}
