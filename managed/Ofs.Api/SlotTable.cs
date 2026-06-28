using System.Collections.Generic;

namespace Ofs
{
    // Process-wide table of registered managed slots, with one specialization per slot type (edit mode /
    // navigator / selection mode / node state). Static so the [UnmanagedCallersOnly] dispatch trampolines —
    // which have no instance — can reach it; a released slot is nulled in place and the table only ever
    // grows, so a slot index stays valid for the process lifetime. The hot-path eval/dispatch trampolines
    // may read Slots without taking Lock (then null-check); every grow/null happens under Lock. Internal —
    // the per-registry wrappers (Editing / Navigation / Selection / Nodes) are the only users, so the
    // public Ofs.Api surface is unchanged.
    internal static class SlotTable<TSlot> where TSlot : class
    {
        public static readonly object Lock = new();
        public static readonly List<TSlot?> Slots = new();

        // Locked, bounds-checked fetch for the dispatch trampolines: a released or out-of-range index → null
        // (which every trampoline treats as "native / nothing to do").
        public static TSlot? Get(int i)
        {
            lock (Lock) { return (i >= 0 && i < Slots.Count) ? Slots[i] : null; }
        }
    }

    // The slots one plugin instance registered into <see cref="SlotTable{TSlot}"/>, tracked so they can be
    // nulled when the plugin unloads — otherwise the static table would pin the plugin's delegates (and
    // thus its collectible AssemblyLoadContext) for the whole process, so the DLL would never unload. One
    // of these replaces the per-registry _ownedSlots list + add-preamble + ReleaseOwnedSlots loop.
    internal sealed class OwnedSlots<TSlot> where TSlot : class
    {
        private readonly List<int> _owned = new();

        // Append `slot` to the shared table and record this instance as its owner; returns the slot index.
        public int Add(TSlot slot)
        {
            lock (SlotTable<TSlot>.Lock)
            {
                int i = SlotTable<TSlot>.Slots.Count;
                SlotTable<TSlot>.Slots.Add(slot);
                _owned.Add(i);
                return i;
            }
        }

        // Null every slot this instance owns (releasing its delegates) and forget them. The table is never
        // shrunk, so indices stay stable for a late or cross-thread trampoline read of an already-released slot.
        public void Release()
        {
            lock (SlotTable<TSlot>.Lock)
            {
                foreach (int i in _owned)
                    if (i >= 0 && i < SlotTable<TSlot>.Slots.Count)
                        SlotTable<TSlot>.Slots[i] = null;
                _owned.Clear();
            }
        }
    }
}
