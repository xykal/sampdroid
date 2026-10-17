package com.xykal.sampdroid.ui

import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.DividerItemDecoration
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.floatingactionbutton.FloatingActionButton
import com.xykal.sampdroid.NativeBridge
import com.xykal.sampdroid.R
import com.xykal.sampdroid.model.ServerEntry
import com.xykal.sampdroid.net.ServerListRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity() {

    private lateinit var adapter: ServerAdapter
    private lateinit var progress: ProgressBar
    private lateinit var status: TextView
    private lateinit var list: RecyclerView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        findViewById<MaterialToolbar>(R.id.toolbar).apply {
            title = "SampDroid"
            subtitle = "native SA-MP client · v" + try {
                NativeBridge.nativeVersion()
            } catch (t: Throwable) {
                "native error: ${t.message}"
            }
        }

        progress = findViewById(R.id.progress)
        status = findViewById(R.id.tvStatus)
        list = findViewById(R.id.recycler)

        adapter = ServerAdapter { openDetail(it) }
        list.layoutManager = LinearLayoutManager(this)
        list.adapter = adapter
        list.addItemDecoration(DividerItemDecoration(this, DividerItemDecoration.VERTICAL))

        findViewById<EditText>(R.id.etSearch).addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) =
                adapter.filter(s?.toString() ?: "")
            override fun afterTextChanged(s: Editable?) = Unit
        })

        findViewById<FloatingActionButton>(R.id.fabRefresh).setOnClickListener { loadServers() }

        loadServers()
    }

    private fun loadServers() {
        progress.visibility = View.VISIBLE
        status.text = "Mengambil daftar server dari open.mp…"
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { ServerListRepository.fetch() }
            }
            progress.visibility = View.GONE
            result.onSuccess { servers ->
                adapter.submit(servers)
                status.text = "${servers.size} server ditemukan"
            }.onFailure { e ->
                status.text = "Gagal memuat: ${e.message}"
            }
        }
    }

    private fun openDetail(server: ServerEntry) {
        ServerDetailDialog.show(this, server)
    }

    companion object {
        const val TAG = "SampDroid"
    }
}
