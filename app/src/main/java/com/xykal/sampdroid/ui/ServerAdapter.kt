package com.xykal.sampdroid.ui

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.xykal.sampdroid.R
import com.xykal.sampdroid.model.ServerEntry

class ServerAdapter(
    private val onClick: (ServerEntry) -> Unit
) : RecyclerView.Adapter<ServerAdapter.VH>() {

    private val items = ArrayList<ServerEntry>()
    private val filtered = ArrayList<ServerEntry>()
    private var query: String = ""

    fun submit(list: List<ServerEntry>) {
        items.clear()
        items.addAll(list)
        applyFilter()
    }

    fun filter(text: String) {
        query = text.trim().lowercase()
        applyFilter()
    }

    private fun applyFilter() {
        filtered.clear()
        if (query.isEmpty()) {
            filtered.addAll(items)
        } else {
            filtered.addAll(items.filter {
                it.hostname.lowercase().contains(query) ||
                    it.gamemode.lowercase().contains(query) ||
                    it.address.contains(query) ||
                    it.language.lowercase().contains(query)
            })
        }
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context).inflate(R.layout.item_server, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(holder: VH, position: Int) = holder.bind(filtered[position])

    override fun getItemCount(): Int = filtered.size

    inner class VH(v: View) : RecyclerView.ViewHolder(v) {
        private val name: TextView = v.findViewById(R.id.tvName)
        private val meta: TextView = v.findViewById(R.id.tvMeta)
        private val count: TextView = v.findViewById(R.id.tvCount)
        private val badge: TextView = v.findViewById(R.id.tvBadge)

        fun bind(s: ServerEntry) {
            name.text = s.hostname
            meta.text = buildString {
                append(s.address)
                if (s.gamemode.isNotBlank()) append("  ·  ").append(s.gamemode)
                if (s.language.isNotBlank()) append("  ·  ").append(s.language)
                if (s.passworded) append("  ·  🔒")
            }
            count.text = "${s.players}/${s.maxPlayers}"
            badge.text = if (s.isOpenMp) "open.mp" else "SA-MP"
            itemView.setOnClickListener { onClick(s) }
        }
    }
}
