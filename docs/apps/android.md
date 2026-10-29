# Android wrapper

nativeTv ships no Android app. The dashboard is a web page, so a phone app can
show it in a full-screen WebView and add what a page cannot do, such as a list
of TVs and Wake-on-LAN. The dashboard recognises such a wrapper by the names
below; an app that uses them gets the navigation described here.

| What | Name |
| :--- | :--- |
| Repository | `nativetv-android` |
| Package | `org.nativetv.android` |
| User agent | contains `NativeTv-Android/<version>` |
| JavaScript interface | `NativeTvApp`, added to the WebView |

## Navigation

The dashboard's sections are a bar along the bottom of a phone screen. When the
user agent names the wrapper and `window.NativeTvApp.navState` is a function,
the page calls

```js
NativeTvApp.navState(false, sectionName, false)
```

each time the section changes. The first argument is whether the page's tabs
are a drawer, which they never are, so the wrapper draws no menu button of its
own. The second is the name of the section on screen. A wrapper whose interface
has no `navState` is left alone.

## Adding a TV

The address and access token of a TV arrive in the link its QR codes carry
(**Settings** tab of the dashboard or the TV app). A wrapper scans one, keeps
the address and token in private storage, excludes them from cloud backups and
opens the TV's dashboard with the token in the link. Without a QR code, the
TV's address is typed; the default port is `8080`.

## Waking a TV

A TV that is off or in standby is woken over the network only if Wake-on-LAN is
on in the TV (**Turn on via Wi-Fi**, or **Mobile TV On** on older webOS
versions). With the TV's MAC address known, a wrapper sends the magic packet to
the local broadcast address until the server answers. A TV in Active Standby
answers the server's `powerOn` API.

## Packages

The dashboard installs a `.ipk` from a file chosen in the page, so a wrapper
that lets the WebView open Android's file picker can sideload packages stored on
the phone.
