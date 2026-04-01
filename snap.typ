#import "head_of_snap.typ": snap-head
#show: snap-head

```rust
    let path = it.path();
    let Some(ext) = path.extension().and_then(|ext| ext.to_str()) else {
      continue;
    };
```
