# Guide

## Setup

Call docProbe once during setup. The unit is `units/probe.service`.

## Example

```
docProbe()
```

## Nested fence

A four-backtick fence holds a three-backtick line; nothing inside is prose.

````
```
fenceProbe()
```
````

## Inline span

````span```` opens no fence, so fenceProbe here is prose.
