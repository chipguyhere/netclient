# Changes from upstream PubSubClient

This library bundles a fork of [PubSubClient](https://github.com/knolleary/pubsubclient)
by Nicholas O'Leary. The forked files live at:

- `src/chipguy_PubSubClient_fork.h`
- `src/chipguy_PubSubClient_fork.cpp`

The upstream license (MIT) is preserved in `LICENSES/PubSubClient.txt` and
the fork's modifications are also released under the MIT License.

## Notable modifications

- **File rename.** Header/source renamed from `PubSubClient.{h,cpp}` to
  `chipguy_PubSubClient_fork.{h,cpp}` so this library can coexist with the
  upstream PubSubClient when both are installed.
- **`mqtt_receive_was_retained` global.** Added a `bool` global (declared in
  the header, defined in the source) that captures the retained-flag bit of
  the incoming PUBLISH packet's header at the two points where the user
  callback is dispatched. The host library reads this immediately after the
  callback returns so it can deliver the retained flag with each received
  message — upstream PubSubClient does not expose this information through
  its callback signature.

If you've made additional modifications, please document them here. A clean
diff against upstream can be produced with:

    git diff <upstream-pubsubclient-version> -- \
        src/chipguy_PubSubClient_fork.h src/chipguy_PubSubClient_fork.cpp
